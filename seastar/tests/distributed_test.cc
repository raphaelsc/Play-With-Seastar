/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include "core/app-template.hh"
#include "core/distributed.hh"
#include "core/future-util.hh"
#include "core/sleep.hh"

struct async : public seastar::async_sharded_service<async> {
    thread_local static bool deleted;
    ~async() {
        deleted = true;
    }
    void run() {
        auto ref = shared_from_this();
        sleep(std::chrono::milliseconds(100 + 100 * engine().cpu_id())).then([this, ref] {
           check();
        });
    }
    virtual void check() {
        assert(!deleted);
    }
    future<> stop() { return make_ready_future<>(); }
};

thread_local bool async::deleted = false;

struct X {
    sstring echo(sstring arg) {
        return arg;
    }
    int cpu_id_squared() const {
        auto id = engine().cpu_id();
        return id * id;
    }
    future<> stop() { return make_ready_future<>(); }
};

template <typename T, typename Func>
future<> do_with_distributed(Func&& func) {
    auto x = make_shared<distributed<T>>();
    return func(*x).finally([x] {
        return x->stop();
    }).finally([x]{});
}

future<> test_that_each_core_gets_the_arguments() {
    return do_with_distributed<X>([] (auto& x) {
        return x.start().then([&x] {
            return x.map_reduce([] (sstring msg){
                if (msg != "hello") {
                    throw std::runtime_error("wrong message");
                }
            }, &X::echo, sstring("hello"));
        });
    });
}

future<> test_functor_version() {
    return do_with_distributed<X>([] (auto& x) {
        return x.start().then([&x] {
            return x.map_reduce([] (sstring msg){
                if (msg != "hello") {
                    throw std::runtime_error("wrong message");
                }
            }, [] (X& x) { return x.echo("hello"); });
        });
    });
}

struct Y {
    sstring s;
    Y(sstring s) : s(std::move(s)) {}
    future<> stop() { return make_ready_future<>(); }
};

future<> test_constructor_argument_is_passed_to_each_core() {
    return do_with_distributed<Y>([] (auto& y) {
        return y.start(sstring("hello")).then([&y] {
            return y.invoke_on_all([] (Y& y) {
                if (y.s != "hello") {
                    throw std::runtime_error(sprint("expected message mismatch, is \"%s\"", y.s));
                }
            });
        });
    });
}

future<> test_map_reduce() {
    return do_with_distributed<X>([] (distributed<X>& x) {
        return x.start().then([&x] {
            return x.map_reduce0(std::mem_fn(&X::cpu_id_squared),
                                 0,
                                 std::plus<int>()).then([] (int result) {
                int n = smp::count - 1;
                if (result != (n * (n + 1) * (2*n + 1)) / 6) {
                    throw std::runtime_error("map_reduce failed");
                }
            });
        });
    });
}

future<> test_async() {
    return do_with_distributed<async>([] (distributed<async>& x) {
        return x.start().then([&x] {
            return x.invoke_on_all(&async::run);
        });
    }).then([] {
        return sleep(std::chrono::milliseconds(100 * (smp::count + 1)));
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return test_that_each_core_gets_the_arguments().then([] {
            return test_functor_version();
        }).then([] {
            return test_constructor_argument_is_passed_to_each_core();
        }).then([] {
            return test_map_reduce();
        }).then([] {
            return test_async();
        });
    });
}
