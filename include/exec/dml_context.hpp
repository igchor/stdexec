#include "../stdexec/execution.hpp"
#include <thread>
#include <iostream>

#include "../stdexec/__detail/__intrusive_queue.hpp"
#include "__detail/__atomic_intrusive_queue.hpp"

#include <dml/dml.hpp>

#include <list>

#include <cstring>

struct dml_context;


// TODO: this is problematic: no way of implementing get_completion_scheduler if we inject the scheduler dynamically???
template <class S>
concept dml_sender = 
  // stdexec::sender<S> &&            
  requires(const S& sndr) {
    {
      stdexec::get_completion_scheduler<stdexec::set_value_t>(stdexec::get_env(sndr))
            .context
    } -> stdexec::__decays_to<dml_context>;
  };

  
template <class R>
concept receiver_with_dml_env = //
  // stdexec::receiver<R> &&          //
  requires(const R& rcvr) {
    {
      stdexec::get_scheduler(stdexec::get_env(rcvr)).context
    } -> stdexec::__decays_to<dml_context*>;
  };

template <typename Desc>
concept DmlDescriptor = requires(Desc desc) {
  std::apply([](auto... params){return dml::submit<dml::automatic>(params...);}, desc);
};

  template <DmlDescriptor Desc>
  auto submit_dml(Desc &&desc) {
    return std::apply([](auto... params){return dml::submit<dml::automatic>(params...);}, std::forward<Desc>(desc));
  }

struct dml_context {
//private:

  struct task : stdexec::__immovable {
      task* next{nullptr};

      virtual void complete() = 0;
      virtual bool is_done() = 0;
  };

  // tasks are either in pending, ready or batch::completion queues.
  using atomic_task_queue = exec::__atomic_intrusive_queue<&task::next>;
  using task_queue = stdexec::__intrusive_queue<&task::next>;

  atomic_task_queue started;
  task_queue pending;

  std::atomic<bool> finishing;

  //////////////////////////// SCHDULER
  struct dml_scheduler {
    // template <stdexec::sender S>
    // friend auto tag_invoke(stdexec::sync_wait_t, const stream_scheduler& self, S&& sndr) {
    //   return sync_wait::sync_wait_t{}(self.context_state_, (S&&) sndr);
    // }

    // friend stdexec::forward_progress_guarantee
    //   tag_invoke(stdexec::get_forward_progress_guarantee_t, const stream_scheduler&) noexcept {
    //   return stdexec::forward_progress_guarantee::weakly_parallel;
    // }

    dml_scheduler(dml_context *context)
      :context(context) {
    }

    // private: TODO
    dml_context* context;

    friend dml_context;

    template <class Receiver>
    struct operation_state : task {
        Receiver receiver;
        dml_context *context;

        operation_state(Receiver&& receiver, dml_context* context)
          : receiver((Receiver&&) receiver), context(context) {
        }

        friend void tag_invoke(stdexec::start_t, operation_state& op) noexcept {
          op.context->pending.push_front(&op);
        }

        void complete() override {
          stdexec::set_value((Receiver&&) receiver);
        }

        bool is_done() override {
          return true;
        }
    };

    struct env {
      dml_context* context;

      dml_scheduler make_scheduler() const {
        return dml_scheduler{context};
      }

      template <class CPO>
      friend dml_scheduler
        tag_invoke(stdexec::get_completion_scheduler_t<CPO>, const env& self) noexcept {
        return self.make_scheduler();
      }
    };

    // template <typename R>
    // struct receiver {
    //     friend void tag_invoke(stdexec::set_value_t, receiver&& self) noexcept
    //     {

    //     }

    //     friend void tag_invoke(stdexec::set_error_t tag, receiver &&self) noexcept
    //     {
          
    //     }
    // };

    struct sender {
      using is_sender = void;

      using completion_signatures = stdexec::
        completion_signatures< stdexec::set_value_t(), stdexec::set_stopped_t() /*, stdexec::set_error_t(std::exception_ptr) */>;

      template <typename Env>
      friend auto tag_invoke(stdexec::get_completion_signatures_t, sender&&, Env)
        noexcept -> completion_signatures;

      template <class R>
      friend auto tag_invoke(stdexec::connect_t, const sender& self, R&& rec) //
        noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<R>, R>) {
        return operation_state(
          (R&&) rec, self.env_.context);
      }

      friend const env& tag_invoke(stdexec::get_env_t, const sender& self) noexcept {
        return self.env_;
      };

        inline sender(dml_context* context) noexcept
        : env_{context} {
      }

      env env_;
    };

    friend inline sender
      tag_invoke(stdexec::schedule_t, const dml_scheduler& self) noexcept {
      return {self.context};
    }

      bool operator==(const dml_scheduler& other) const noexcept {
        // TODO: come up with something better
        return context == other.context;
      }
  };

  auto get_scheduler() {
    return dml_scheduler{this};
  }
  //////////////////////////////////////

    struct env {
      dml_context* context;

      dml_scheduler make_scheduler() const {
        return dml_scheduler{context};
      }

      template <class CPO>
      friend dml_scheduler
        tag_invoke(stdexec::get_completion_scheduler_t<CPO>, const env& self) noexcept {
        return self.make_scheduler();
      }
    };

  template <typename Receiver, DmlDescriptor Descriptor>
  struct dml_operation : task {
    dml_operation(Receiver rec, dml_context *context, Descriptor desc):
      rec(rec), context(context), desc(desc) {
      }

    void complete() override {
      // TODO: add return value + error checking ...
      stdexec::set_value((Receiver&&) rec);
    }

    bool is_done() override {
      return this->handle.is_finished();
    }

    friend void tag_invoke(stdexec::start_t, dml_operation &self) noexcept {
      self.handle = submit_dml(self.desc);
      self.context->started.push_front(&self);
    }

    private:
      Receiver rec;
      dml_context *context;
      Descriptor desc;
      decltype(submit_dml(std::declval<Descriptor>())) handle;
  };

  template <DmlDescriptor Descriptor>
  struct memory_operation_sender {
    using is_sender = void;

    // friend const env& tag_invoke(stdexec::get_env_t, const memory_operation_sender& self) noexcept {
    //   return self.env_;
    // };
    
    memory_operation_sender(dml_context* context, Descriptor desc): context(context), desc(desc), env_(context) {}

    template <typename Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t, memory_operation_sender&&, Env)
      noexcept -> stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;

    template <typename Receiver>
    friend dml_operation<Receiver, Descriptor> tag_invoke(stdexec::connect_t, memory_operation_sender self, Receiver &&r) noexcept {
      assert(self.context); // TODO this should be compile-time
      return dml_operation<Receiver, Descriptor>((Receiver&&)(r), self.context, self.desc);
    }

    dml_context* context;
    Descriptor desc;
    env env_;
  };

  template <DmlDescriptor Descriptor>
  struct memory_operation_dynamic_sender {
    using is_sender = void;
    
    memory_operation_dynamic_sender(Descriptor desc): desc(desc) {}

    template <typename Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t, memory_operation_dynamic_sender&&, Env)
      noexcept -> stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;

    template <receiver_with_dml_env Receiver>
    friend dml_operation<Receiver, Descriptor> tag_invoke(stdexec::connect_t, memory_operation_dynamic_sender self, Receiver &&r) noexcept {
      return dml_operation<Receiver, Descriptor>((Receiver&&)(r), stdexec::get_scheduler(stdexec::get_env(r)).context, self.desc);
    }

    Descriptor desc;
  };

  static auto async_memcpy(dml_context &context, void *src, void* dst, size_t size) noexcept {
    return memory_operation_sender(&context, std::make_tuple(dml::mem_move, dml::make_view(src, size), dml::make_view(dst, size)));
  }

  static auto async_memcpy_dynamic(void *src, void* dst, size_t size) noexcept {
    return memory_operation_dynamic_sender(std::make_tuple(dml::mem_move, dml::make_view(src, size), dml::make_view(dst, size)));
  }

  template <typename Receiver, DmlDescriptor... Descriptors>
  struct when_all_dml_operation : task {
    when_all_dml_operation(Receiver rec, dml_context *context, std::tuple<Descriptors...> descriptors):
      rec(rec), context(context), descriptors(descriptors) {}

    void complete() override {
      // TODO: add return value + error checking ...
      stdexec::set_value((Receiver&&) rec);
    }

    bool is_done() override {
      return this->handle.is_finished();
    }

    friend void tag_invoke(stdexec::start_t, when_all_dml_operation &self) noexcept {
      auto sequence = dml::sequence(sizeof...(Descriptors), std::allocator<dml::byte_t>{});

      auto add_to_sequence = [&sequence](auto desc){ std::apply([&sequence](auto... params){sequence.add(params...);}, desc); };
      std::apply([&add_to_sequence](auto&... desc){(..., add_to_sequence(desc));}, self.descriptors);

      self.handle = dml::submit<dml::automatic, std::allocator<dml::byte_t>>(dml::batch, sequence);
      self.context->started.push_front(&self);
    }

    private:
      Receiver rec;
      dml_context *context;
      std::tuple<Descriptors...> descriptors;
      dml::handler<dml::batch_operation, std::allocator<dml::byte_t>> handle;
  };

  template <DmlDescriptor... Descriptors>
  struct when_all_sender {
    using is_sender = void;

    friend const env& tag_invoke(stdexec::get_env_t, const when_all_sender& self) noexcept {
      return self.env_;
    };
    
    when_all_sender(dml_context* context, std::tuple<Descriptors...> desc): context(context), desc(desc), env_(context) {}

    template <typename Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t, when_all_sender&&, Env)
      noexcept -> stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename Receiver>
    friend when_all_dml_operation<Receiver, Descriptors...> tag_invoke(stdexec::connect_t, when_all_sender self, Receiver &&r) noexcept {
      return when_all_dml_operation<Receiver, Descriptors...>((Receiver&&)(r), self.context, self.desc);
    }

  private:
    dml_context* context;
    std::tuple<Descriptors...> desc;
    env env_;
  };

  template <typename... Senders>
  friend auto tag_invoke(stdexec::when_all_t, Senders&&... senders) noexcept {
    auto get_descriptor = [](auto& sender) { return sender.desc; };
    auto descriptors = std::make_tuple(get_descriptor(senders)...);

    // TODO: this where get_env should be used?
    auto s = std::make_tuple(senders...);
    auto context = std::get<0>(s).context;

    return when_all_sender(context, descriptors);
  }

 std::pair<task_queue, task_queue> check_and_complete(task_queue &&pending) const {
    task_queue still_pending;
    task_queue ready;
    while (!pending.empty()) {
      auto task = pending.pop_front();
      if (task->is_done()) {
        ready.push_front(task);
      } else {
        still_pending.push_front(task);
      }
    }
    return std::pair<task_queue, task_queue>{std::move(ready), std::move(still_pending)};
  }

  void complete(task_queue &&ready) const {
    while (!ready.empty()) {
      ready.pop_front()->complete();
    }
  }

  void run() {
    while (!finishing.load()) {
      auto [ready, pending] = check_and_complete(std::move(this->pending));

      complete(std::move(ready));

      this->pending = std::move(pending);
      this->pending.append(started.pop_all());
    }
  }

  void finish() {
    finishing.store(true);
  }
};

