/*
 * Copyright (c) 2023 Intel Corporation
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Use a thread pool
#include "exec/static_thread_pool.hpp"

#include "exec/dml_context.hpp"

#include "exec/when_any.hpp"
#include "exec/finally.hpp"

#include "stdexec/execution.hpp"

#include <thread>

#include <iostream>


template <typename R>
struct my_op :stdexec::__immovable {
  R r;

  explicit my_op(R&& r): r((R&&) r) {}

  friend void tag_invoke(stdexec::start_t, my_op &self) noexcept {
    stdexec::set_value((R&&) self.r);
  }
};

struct my_sender {
  void *ptr;
  void *sss;
  
  template <typename Env>
  friend auto tag_invoke(stdexec::get_completion_signatures_t, my_sender&&, Env)
   noexcept -> stdexec::completion_signatures<stdexec::set_value_t()>;

  template <typename Receiver>
  friend my_op<Receiver> tag_invoke(stdexec::connect_t, my_sender self, Receiver &&r) noexcept {
    return my_op<Receiver>((Receiver&&)(r));
  }
};


int main() {
  polling_context context;
  std::thread dml_thread{[&] {
    context.run();
  }};

  std::vector<uint8_t> src1(8, 0);
  std::vector<uint8_t> src2(8, 1);

  std::vector<uint8_t> dst1(8);
  std::vector<uint8_t> dst2(8);

  exec::static_thread_pool work_pool{8};
  auto work_sched = work_pool.get_scheduler();

  auto just_cout = stdexec::just() | stdexec::then([&]{std::cout << "then tid: " << std::this_thread::get_id() << std::endl; return 1;});

  // stdexec::sync_wait(
  //   stdexec::when_all(
  //       stdexec::on(work_sched, just_cout) | stdexec::let_value([&](int a){std::cout << a << " let value: " << std::this_thread::get_id() << std::endl; return stdexec::just();}),
  //       polling_context::async_memcpy(context, src1.data(), dst1.data(), 8) | stdexec::let_value([&](auto...){std::cout << " let value after memcpy: " << std::this_thread::get_id() << std::endl; return stdexec::just();}),
  //       polling_context::async_memcpy(context, src2.data(), dst2.data(), 8))
  //   | stdexec::then([&]{std::cout << "Src and dst buffers " << (src1 == dst1 && src2 == dst2) << " . tid: " << std::this_thread::get_id() << std::endl;}));

   stdexec::sync_wait(
    stdexec::when_all(
        polling_context::async_memcpy(context, src1.data(), dst1.data(), 8),
        polling_context::async_memcpy(context, src2.data(), dst2.data(), 8))
    | stdexec::then([&]{std::cout << "Src and dst buffers " << (src1 == dst1 && src2 == dst2) << " . tid: " << std::this_thread::get_id() << std::endl;}));

  // stdexec::sync_wait(polling_context::async_memcpy(context, src1.data(), dst1.data(), 8));
  // stdexec::sync_wait(my_sender{});

  context.finish();
  dml_thread.join();
}
