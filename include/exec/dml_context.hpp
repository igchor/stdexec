#include "../stdexec/execution.hpp"
#include <thread>
#include <iostream>

#include "../stdexec/__detail/__intrusive_queue.hpp"
#include "__detail/__atomic_intrusive_queue.hpp"

#include <dml/dml.hpp>

#include <list>

#include <cstring>

// dynamic operation batch
  // TODO: also make static one?
  // TODO: create base class for non-dml usages?
  // struct batch_t {
  //   batch_t(task_queue queue): sequence(queue.) {}

  //   decltype(dml::sequence(0, std::allocator<dml::byte_t>{})) sequence;
  //   batch_t* next{nullptr};

  //   template <typename Descriptor>
  //   void add(task* op, Descriptor desc) {
  //     completion.push_back(op);
  //     //ops.push_back({src, dst, size});
  //     auto status = sequence.add(dml::mem_move, dml::make_view(src, size), dml::make_view(dst, size));

  //     if (status != dml::status_code::ok)
  //       throw std::runtime_error("Sequence::add failed");
  //   }

  //   void submit() {
  //     // for (auto [src, dst, size] : ops) {
  //     //   memcpy(src, dst, size);
  //     // }
  //     // handler = dml::submit<dml::software>(dml::batch, sequence);
  //     if (!handler.valid()) {
  //       std::cout << (int) handler.get().status << std::endl;
  //       throw std::runtime_error("handler invalid");
  //     }
  //   }

  //   bool is_ready() {
  //     //return true;
  //     return handler.is_finished();
  //   }

  //   void complete() {
  //     // TODO: check for errors...
  //     assert(is_ready());
  //     while (!completion.empty()) {
  //       completion.pop_front()->complete();
  //     }
  //   }
  // };

  // // TODO: use some kind of a ringbuffer? size based onmax possible hardware (DSA) capability
  // std::list<batch_t> batches;

template <typename Desc>
concept DmlDescriptor = requires(Desc desc) {
  std::apply([](auto... params){return dml::submit<dml::automatic>(params...);}, desc);
};

template <typename Sender>
concept dmlSender = requires(Sender sender) {
  sender.desc;
};

struct polling_context {
//private:
  struct batch_t;

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

  // template <typename Receiver>
  // struct schedule_operation : task {
  //   Receiver rec;
  //   polling_context &ctx;

  //   schedule_operation(Receiver rec, polling_context &ctx): rec(rec), ctx(ctx) {}

  //   void complete() override {
  //     std::cout << "schedule_operation complete " << std::this_thread::get_id() << std::endl;
  //     stdexec::set_value((Receiver&&) rec);
  //   }

  //   friend void tag_invoke(stdexec::start_t, schedule_operation &self) noexcept {
  //     self.ctx.ready.push_front(&self);
  //   }
  // };

  // struct schedule_sender {
  //   using is_sender = void;

  //   template <typename Env>
  //   friend auto tag_invoke(stdexec::get_completion_signatures_t, schedule_sender&&, Env)
  //     noexcept -> stdexec::completion_signatures<stdexec::set_value_t()>;

  //   polling_context* ctx;

  //   template <typename Receiver>
  //   friend schedule_operation<Receiver> tag_invoke(stdexec::connect_t, schedule_sender self, Receiver &&r) noexcept {
  //     return schedule_operation<Receiver>((Receiver&&)(r), *self.ctx);
  //   }
  // };

  // struct scheduler {
  //   polling_context* ctx;
  //   friend schedule_sender tag_invoke(stdexec::schedule_t, const scheduler& self) noexcept {
  //     return {self.ctx};
  //   }
  // };

  // scheduler get_scheduler() noexcept {
  //   return {this};
  // }

  template <typename Receiver, DmlDescriptor Descriptor>
  struct dml_operation : task {
    dml_operation(Receiver rec, polling_context *ctx, Descriptor desc):
      rec(rec), ctx(ctx), desc(desc) {}

    void complete() override {
      // TODO: add return value + error checking ...
      stdexec::set_value((Receiver&&) rec);
    }

    bool is_done() override {
      return this->handle.is_finished();
    }

    friend void tag_invoke(stdexec::start_t, dml_operation &self) noexcept {
      std::cout << "NORMAL" << std::endl;
      self.handle = std::apply([](auto... params){return dml::submit<dml::automatic>(params...);}, self.desc);
      self.ctx->started.push_front(&self);
    }

    private:
      Receiver rec;
      polling_context *ctx;
      Descriptor desc;
      decltype(std::apply([](auto... params){return dml::submit<dml::automatic>(params...);}, std::declval<Descriptor>())) handle;
  };

  template <typename Descriptor>
  struct memory_operation_sender {
    using is_sender = void;
    
    memory_operation_sender(polling_context* ctx, Descriptor desc): ctx(ctx), desc(desc) {}

    template <typename Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t, memory_operation_sender&&, Env)
      noexcept -> stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename Receiver>
    friend dml_operation<Receiver, Descriptor> tag_invoke(stdexec::connect_t, memory_operation_sender self, Receiver &&r) noexcept {
      return dml_operation<Receiver, Descriptor>((Receiver&&)(r), self.ctx, self.desc);
    }

    polling_context* ctx;
    Descriptor desc;
  };

  // TODO: async_memcpy should be a CPO?
  // Get rid of ctx -> get it from sender->get_completion_scheduler?
  static auto async_memcpy(polling_context &ctx, void *src, void* dst, size_t size) noexcept {
    return memory_operation_sender(&ctx, std::make_tuple(dml::mem_move, dml::make_view(src, size), dml::make_view(dst, size)));
  }

  template <typename Receiver, DmlDescriptor... Descriptors>
  struct when_all_dml_operation : task {
    when_all_dml_operation(Receiver rec, polling_context *ctx, std::tuple<Descriptors...> descriptors):
      rec(rec), ctx(ctx), descriptors(descriptors) {}

    void complete() override {
      // TODO: add return value + error checking ...
      stdexec::set_value((Receiver&&) rec);
    }

    bool is_done() override {
      return this->handle.is_finished();
    }

    friend void tag_invoke(stdexec::start_t, when_all_dml_operation &self) noexcept {
      std::cout << "WHEN ALL" << std::endl;
      auto sequence = dml::sequence(sizeof...(Descriptors), std::allocator<dml::byte_t>{});

      auto add_to_sequence = [&sequence](auto desc){ std::apply([&sequence](auto... params){sequence.add(params...);}, desc); };
      std::apply([&add_to_sequence](auto&... desc){(..., add_to_sequence(desc));}, self.descriptors);

      self.handle = dml::submit<dml::automatic, std::allocator<dml::byte_t>>(dml::batch, sequence);
      self.ctx->started.push_front(&self);
    }

    private:
      Receiver rec;
      polling_context *ctx;
      std::tuple<Descriptors...> descriptors;
      dml::handler<dml::batch_operation, std::allocator<dml::byte_t>> handle;
  };

  template <DmlDescriptor... Descriptors>
  struct when_all_sender {
    using is_sender = void;
    
    when_all_sender(polling_context* ctx, std::tuple<Descriptors...> desc): ctx(ctx), desc(desc) {}

    template <typename Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t, when_all_sender&&, Env)
      noexcept -> stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename Receiver>
    friend when_all_dml_operation<Receiver, Descriptors...> tag_invoke(stdexec::connect_t, when_all_sender self, Receiver &&r) noexcept {
      return when_all_dml_operation<Receiver, Descriptors...>((Receiver&&)(r), self.ctx, self.desc);
    }

  private:
    polling_context* ctx;
    std::tuple<Descriptors...> desc;
  };

  template <typename... Senders>
  requires dmlSender<std::common_type_t<Senders...>> // or implement fallback for others?
  friend auto tag_invoke(stdexec::when_all_t, Senders&&... senders) noexcept {
    // isnt it too late to careate a batch here????
    auto get_descriptor = [](auto& sender) { return sender.desc; };
    auto descriptors = std::make_tuple(get_descriptor(senders)...);

    // TODO: this where get_env should be used?
    auto s = std::make_tuple(senders...);
    auto ctx = std::get<0>(s).ctx;

    return when_all_sender(ctx, descriptors);
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

