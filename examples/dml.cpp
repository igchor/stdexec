// Use a thread pool
#include "exec/static_thread_pool.hpp"

#include "exec/dml_context.hpp"

#include "exec/when_any.hpp"
#include "exec/finally.hpp"
#include "stdexec/execution.hpp"
#include <thread>
#include <iostream>

// auto cachelib_use_case1() {
//   auto get_ptr = []{
//     std::cout << "get_ptr" << std::endl;
//     return std::pair<void*> {
//       (void*) 0x1234,
//       (void*) 0x1234
//     };
//   };

//   auto finish = [](void *old_ptr){
//     // cleanup after ptr
//     std::cout << "cleanup: " << old_ptr << std::endl;
//   };

//   auto async_memcpy = [](std::pair<void*> ptrs) {
//     std::cout << "mempcy: " << ptrs.first << " " << ptrs.second << std::endl;
//     return stdexec::just(ptrs.second);
//   };

//   auto get_ptr_sender = stdexec::then(stdexec::just(), get_ptr);
//   auto memcpy_sender = stdexec::let_value(get_ptr_sender, async_memcpy);
//   auto complete_sender = stdexec::then(memcpy_sender, finish);

//   return stdexec::when_all(
//     complete_sender,
//     complete_sender,
//     complete_sender
//   );
// }

// auto cachelib_use_case2() {
//   auto get_ptrs = []{
//     std::cout << "get_ptr" << std::endl;
//     return std::pair<std::vector<void*>, std::vector<void*>> {
//        {(void*) 0x1234, (void*) 0x1234, (void*) 0x1234, (void*) 0x1234},
//        {(void*) 0x1234, (void*) 0x1234, (void*) 0x1234, (void*) 0x1234}
//     };
//   };

//   auto finish = [](){ // todo, use whan_all_variant or something to pass values
//     // cleanup after ptr
//     std::cout << "cleanup: " << std::endl;
//   };

//   auto async_memcpy = [](std::pair<std::vector<void*>, std::vector<void*>> ptrs) {
//     auto async_memcpy_sender_f = [](void *ptr, void *new_ptr) {
//       return stdexec::just(); // TODO, return actual sender
//     };

//     return stdexec::when_all( // TODO, how to make it dynamic? Need another CPO for when_all -> size + range of senders? (or a generator/function that takes index to generate those senders?)
//       async_memcpy_sender_f(ptrs.first[0], ptrs.second[0]),
//       async_memcpy_sender_f(ptrs.first[1], ptrs.second[1])
//     );
//   };

//   auto get_ptr_sender = stdexec::then(stdexec::just(), get_ptrs); // TODO or use BULK here
//   auto memcpy_sender = stdexec::let_value(get_ptr_sender, async_memcpy_sender);
//   auto complete_sender = stdexec::then(memcpy_sender, finish); // TODO or use BULK here

//   return stdexec::when_all(
//     complete_sender,
//     complete_sender,
//     complete_sender
//   );
// }


int main() {
  dml_context context;
  std::thread dml_thread{[&] {
    context.run();
  }};

  auto sched = context.get_scheduler();

  std::vector<uint8_t> src1(8, 0);
  std::vector<uint8_t> src2(8, 1);

  std::vector<uint8_t> dst1(8);
  std::vector<uint8_t> dst2(8);

  //  stdexec::sync_wait(
  //   stdexec::when_all(
  //       dml_context::async_memcpy(context, src1.data(), dst1.data(), 8) | stdexec::then([]{std::cout << "then" << std::endl;}) | stdexec::let_value([&](auto...) { return dml_context::async_memcpy(context, src1.data(), dst1.data(), 8); }),
  //       dml_context::async_memcpy(context, src2.data(), dst2.data(), 8))
  //   | stdexec::then([&]{std::cout << "Src and dst buffers " << (src1 == dst1 && src2 == dst2) << " . tid: " << std::this_thread::get_id() << std::endl;}));

  // auto snd = stdexec::when_all(
  //       stdexec::on(sched, dml_context::async_memcpy(context, src1.data(), dst1.data(), 8)),
  //       stdexec::on(sched, dml_context::async_memcpy(context, src2.data(), dst2.data(), 8))
  //   );

  // exec::static_thread_pool p;
  // auto schsed = p.get_scheduler();

  stdexec::sync_wait(
    stdexec::on(sched, dml_context::async_memcpy_dynamic(src1.data(), dst1.data(), 8))
  );

  // stdexec::sync_wait(
  //   stdexec::on(sched, dml_context::async_memcpy_dynamic(src1.data(), dst1.data(), 8))
  // );

  // stdexec::sync_wait(
  //   stdexec::when_all(
  //     stdexec::on(sched, dml_context::async_memcpy_dynamic(src1.data(), dst1.data(), 8)),
  //     stdexec::on(sched, dml_context::async_memcpy_dynamic(src2.data(), dst2.data(), 8))
  // ));

  // using Sender = dml_context::memory_operation_sender<std::tuple<dml::mem_move_operation, dml::data_view, dml::data_view> >;
  // using Receiver = stdexec::__on::__receiver_ref<stdexec::_Yp<dml_context::dml_scheduler>, stdexec::_Yp<dml_context::memory_operation_sender<std::tuple<dml::mem_move_operation, dml::data_view, dml::data_view> > >, stdexec::_Yp<stdexec::__compl_sigs::__env_promise<stdexec::__sync_wait::__env> > >::__t;

  //static_assert(stdexec::__connect::__connectable_with_tag_invoke<S1, R1>);


  // stdexec::sync_wait(
  //   snd  
  // );

  context.finish();
  dml_thread.join();
}
