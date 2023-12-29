/*
 * Copyright (c) 2023 XXX
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

#include <iostream>

#include <tbbexec/tbb_thread_pool.hpp>
#include <exec/static_thread_pool.hpp>

#include <exec/any_sender_of.hpp>
#include <exec/variant_sender.hpp>
#include <stdexec/execution.hpp>

long serial_fib(int n) {
    return n < 2 ? n : serial_fib(n - 1) + serial_fib (n - 2);
}

template <class... Ts>
  using any_sender_of =
    typename exec::any_receiver_ref<stdexec::completion_signatures<Ts...>>::template any_sender<>;

using fib_sender = any_sender_of<stdexec::set_value_t(long), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

template <class ThenSender, class ElseSender>
exec::variant_sender<ThenSender, ElseSender>
  if_then_else(bool condition, ThenSender then, ElseSender otherwise) {
  if (condition) {
    return then;
  }
  return otherwise;
}

template <class S, class R>
using connect_result_t = decltype(connect(std::declval<S>(), std::declval<R>()));

template <typename Scheduler>
struct fib_s {
    using sender_concept = stdexec::sender_t;
    using Scheduler_t = Scheduler;
    using __t = fib_s;
    using __id = fib_s;
    using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(long), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

    int cutoff;
    int n;
    Scheduler sched;

    template <class Receiver>
    struct operation {
      Receiver rcvr_;
      int cutoff;
      long n;
      Scheduler sched;

      friend void tag_invoke(stdexec::start_t, operation& self) noexcept {
            if (self.n < self.cutoff) {
                stdexec::set_value((Receiver&&) self.rcvr_, serial_fib(self.n));
            } else {
                fib_s<Scheduler> child1{self.cutoff, self.n - 1, self.sched};
                fib_s<Scheduler> child2{self.cutoff, self.n - 2, self.sched};

                stdexec::start_detached(
                    stdexec::on(self.sched, stdexec::when_all(
                        fib_sender(child1),
                        fib_sender(child2)
                    ) | stdexec::then([rcvr = std::move(self.rcvr_)](long a, long b) {
                        stdexec::set_value((Receiver&&) rcvr, a + b);
                    })
                ));

                // auto snd = 
                // stdexec::schedule(self.sched) 
                // // stdexec::when_all(
                // //     stdexec::on(self.sched, child1),
                // //     stdexec::on(self.sched, child2)
                // // )
                //  | stdexec::then([]() { // lifetime??
                //     // stdexec::set_value((Receiver&&) rcvr, a + b);
                //     // delete child1;
                //     // delete child2;
                //     return 100;
                // });

                // auto r = receiver<Receiver>{(Receiver&&) self.rcvr_, self.cutoff, self.n, self.sched};

                // using al = decltype(stdexec::connect(std::move(snd2), std::move(r)));

                // auto op = new al(stdexec::connect(std::move(snd2), std::move(r)));
                // stdexec::start(*op);
            }
        }
    };

    template <stdexec::receiver_of<completion_signatures> Receiver>
    friend operation<Receiver> tag_invoke(stdexec::connect_t, fib_s self, Receiver rcvr) {
      return {(Receiver&&) rcvr, self.cutoff, self.n, self.sched};
    }
};

template <typename Scheduler>
fib_sender fib(Scheduler scheduler, int cutoff, int n) {
    return if_then_else(n < cutoff,
            stdexec::just(serial_fib(n)),
            stdexec::let_value(
                stdexec::just(n),
                [cutoff, scheduler](int n) {
                    return stdexec::when_all(
                        stdexec::on(scheduler, fib(scheduler, cutoff, n - 1)),
                        stdexec::on(scheduler, fib(scheduler, cutoff, n - 2))
                    ) | stdexec::then([](long a, long b) {
                        return a + b;
                    });
                })
    );
}

template <typename Scheduler>
auto fib_custom_sender(Scheduler scheduler, int cutoff, int n) {
    return stdexec::on(scheduler, fib_s<Scheduler>{cutoff, n, scheduler});
}

template <typename duration, typename F>
auto measure(F&& f) {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration_cast<duration>(std::chrono::steady_clock::now() - start).count();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "ERROR" << std::endl;
        return -1;
    }

    int cutoff = std::atoi(argv[1]);
    int n = std::atoi(argv[2]);

    // std::variant<tbbexec::tbb_thread_pool, exec::static_thread_pool> pool;
    
    if (argv[3] == std::string_view("tbb")) {
        //pool.emplace<tbbexec::tbb_thread_pool>(std::thread::hardware_concurrency());
        tbbexec::tbb_thread_pool pool(std::thread::hardware_concurrency());

        auto snd = fib_custom_sender(pool.get_scheduler(), cutoff, n);

        long result;
        auto time = measure<std::chrono::milliseconds>([&]{
            tbb::task_group tg;
            tg.run_and_wait([&]{
                auto [value] = stdexec::sync_wait(std::move(snd)).value();
                result = value;
            });
        }); 

        std::cout << time << " us. Value: " << result << std::endl;
    } else {
        //pool.emplace<exec::static_thread_pool>(std::thread::hardware_concurrency(), exec::bwos_params{}, exec::get_numa_policy());
        exec::static_thread_pool pool(std::thread::hardware_concurrency(), exec::bwos_params{}, exec::get_numa_policy());

        auto snd = fib_custom_sender(pool.get_scheduler(), cutoff, n);

        long result;
        auto time = measure<std::chrono::milliseconds>([&]{
            auto [value] = stdexec::sync_wait(std::move(snd)).value();
            result = value;
        });

        std::cout << time << " us. Value: " << result << std::endl;
    }

    // auto snd = std::visit([&](auto &&pool) {
    //     return fib2(pool.get_scheduler(), cutoff, n);
    // }, pool);

    // long result;
    // auto time = measure<std::chrono::milliseconds>([&]{
    //     tbb::task_group tg;
    //     tg.run_and_wait([&]{
    //         auto [value] = stdexec::sync_wait(std::move(snd)).value();
    //         result = value;
    //     });
    // });   

    // std::cout << time << " us. Value: " << result << std::endl;
}
