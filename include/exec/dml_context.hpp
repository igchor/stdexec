#include "../stdexec/execution.hpp"
#include <thread>
#include <iostream>

#include "../stdexec/__detail/__intrusive_queue.hpp"
#include "__detail/__atomic_intrusive_queue.hpp"

#include <dml/dml.hpp>

#include <list>

#include <cstring>

struct polling_context {
//private:
  struct batch_t;

  struct task : stdexec::__immovable {
      task* next{nullptr};

      virtual void complete() = 0;
      virtual void submit(batch_t&){};
  };

  // tasks are either in pending, ready or batch::completion queues.
  using atomic_task_queue = exec::__atomic_intrusive_queue<&task::next>;
  using task_queue = stdexec::__intrusive_queue<&task::next>;

  atomic_task_queue pending;
  atomic_task_queue ready;

  std::atomic<bool> finishing;

  template <typename Receiver>
  struct schedule_operation : task {
    Receiver rec;
    polling_context &ctx;

    schedule_operation(Receiver rec, polling_context &ctx): rec(rec), ctx(ctx) {}

    void complete() override {
      stdexec::set_value((Receiver&&) rec);
    }

    friend void tag_invoke(stdexec::start_t, schedule_operation &self) noexcept {
      self.ctx.ready.push_front(&self);
    }
  };

  // wrapper around dml sequance, stores completion callbacks and link to other batches
  struct batch_t {
    batch_t(size_t length)/*: sequence(length)*/ {}

    // TODO: better to use C API?
    //dml::sequence<> sequence;
    //dml::handler<dml::batch_operation, std::allocator<dml::byte_t>> handler; // TODO: generalize
    dml::handler<dml::mem_move_operation, std::allocator<dml::byte_t>> handler;
    task_queue completion;
    batch_t* next{nullptr};

    // std::list<std::tuple<void*, void*, size_t>> ops;

    // TODO: generalize for different ops, and use dml types?
    void add(task* op, void* src, void *dst, size_t size) {
      completion.push_back(op);
      //ops.push_back({src, dst, size});
      //auto status = sequence.add(dml::mem_move, dml::make_view(src, size), dml::make_view(dst, size));
      handler = dml::submit<dml::software>(dml::mem_move, dml::make_view(src, size), dml::make_view(dst, size));
      // if (status != dml::status_code::ok) throw std::runtime_error("TODO");
    }

    void submit() {
      // for (auto [src, dst, size] : ops) {
      //   memcpy(src, dst, size);
      // }
      // handler = dml::submit<dml::software>(dml::batch, sequence);
      if (!handler.valid()) {
        std::cout << (int) handler.get().status << std::endl;
        throw std::runtime_error("handler invalid");
      }
    }

    bool is_ready() {
      //return true;
      return handler.is_finished();
    }

    void complete() {
      // TODO: check for errors...
      assert(is_ready());
      while (!completion.empty()) {
        completion.pop_front()->complete();
      }
    }
  };

  // TODO: this might not be needed if we'd use C API / refined C++ api
  // Just embed the memory on which we wait into the task/operation
  std::list<batch_t> batches;

  template <typename Receiver>
  struct memory_operation : task {
    Receiver rec;
    void *src, *dst;
    size_t size;
    polling_context &ctx;

    memory_operation(Receiver rec, void *src, void *dst, size_t size, polling_context &ctx): rec(rec), src(src), dst(dst), size(size), ctx(ctx) {}

    void complete() override {
      stdexec::set_value((Receiver&&) rec);
    }

    void submit(batch_t& batch) override {
      batch.add(this, src, dst, size);
    }

    friend void tag_invoke(stdexec::start_t, memory_operation &self) noexcept {
      self.ctx.pending.push_front(&self);
    }
  };

  struct schedule_sender {
    using is_sender = void;

    template <typename Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t, schedule_sender&&, Env)
      noexcept -> stdexec::completion_signatures<stdexec::set_value_t()>;

    polling_context* ctx;

    template <typename Receiver>
    friend schedule_operation<Receiver> tag_invoke(stdexec::connect_t, schedule_sender self, Receiver &&r) noexcept {
      return schedule_operation<Receiver>((Receiver&&)(r), *self.ctx);
    }
  };

  struct memory_operation_sender {
    using is_sender = void;
    
    template <typename Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t, memory_operation_sender&&, Env)
      noexcept -> stdexec::completion_signatures<stdexec::set_value_t()>;

    void *src, *dst;
    size_t size;
    polling_context* ctx;

    template <typename Receiver>
    friend memory_operation<Receiver> tag_invoke(stdexec::connect_t, memory_operation_sender self, Receiver &&r) noexcept {
      return memory_operation<Receiver>((Receiver&&)(r), self.src, self.dst, self.size, *self.ctx);
    }
  };

  struct scheduler {
    polling_context* ctx;
    friend schedule_sender tag_invoke(stdexec::schedule_t, const scheduler& self) noexcept {
      return {self.ctx};
    }
  };

  scheduler get_scheduler() noexcept {
    return {this};
  }

  // TODO: async_memcpy should be a CPO? define concepts?
  static memory_operation_sender async_memcpy(scheduler sched, void *src, void* dst, size_t size) noexcept {
    return memory_operation_sender{src, dst, size, sched.ctx};
  }

  void complete() {
    auto to_complete = ready.pop_all();
    while (!to_complete.empty()) {
      to_complete.pop_front()->complete();
    }
  }

  void check_batches() {
    // TODO: effectively wait for the batches to finish
    for (auto it = batches.begin(); it != batches.end();) {
      if (it->is_ready()) {
        it->complete();
        it = batches.erase(it);
      } else {
        ++it; 
      }
    }
  }

  void submit_as_batch(task_queue queue) {
    // TODO: use C api or calculate length first?
    static constexpr size_t batch_size = 1;

    while (!queue.empty()) {
      auto &batch = batches.emplace_back(batch_size);
      
      size_t num_submitted = 0;
      while (!queue.empty() && num_submitted < batch_size) {
        queue.pop_front()->submit(batch);
        num_submitted++;
      }
      
      batch.submit();
    }
  }

  void run() {
    while (!finishing.load()) {
      complete();
      check_batches();
      submit_as_batch(pending.pop_all());
    }
  }

  // TODO: specialization of when_all for memory operation??? or some other way of optimizing for batch processing

  void finish() {
    finishing.store(true);
  }
};

