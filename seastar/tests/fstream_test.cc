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

#include <algorithm>
#include <iostream>
#include <numeric>
#include "core/reactor.hh"
#include "core/fstream.hh"
#include "core/shared_ptr.hh"
#include "core/app-template.hh"
#include "core/do_with.hh"
#include "core/seastar.hh"
#include "test-utils.hh"
#include "core/thread.hh"
#include <random>
#include <boost/range/adaptor/transformed.hpp>

struct writer {
    output_stream<char> out;
    writer(file f) : out(make_file_output_stream(std::move(f))) {}
};

struct reader {
    input_stream<char> in;
    reader(file f) : in(make_file_input_stream(std::move(f))) {}
};

SEASTAR_TEST_CASE(test_fstream) {
    auto sem = make_lw_shared<semaphore>(0);

        open_file_dma("testfile.tmp",
                      open_flags::rw | open_flags::create | open_flags::truncate).then([sem] (file f) {
            auto w = make_shared<writer>(std::move(f));
            auto buf = static_cast<char*>(::malloc(4096));
            memset(buf, 0, 4096);
            buf[0] = '[';
            buf[1] = 'A';
            buf[4095] = ']';
            w->out.write(buf, 4096).then([buf, w] {
                ::free(buf);
                return make_ready_future<>();
            }).then([w] {
                auto buf = static_cast<char*>(::malloc(8192));
                memset(buf, 0, 8192);
                buf[0] = '[';
                buf[1] = 'B';
                buf[8191] = ']';
                return w->out.write(buf, 8192).then([buf, w] {
                    ::free(buf);
                    return w->out.close().then([w] {});
                });
            }).then([] {
                return open_file_dma("testfile.tmp", open_flags::ro);
            }).then([] (file f) {
                /*  file content after running the above:
                 * 00000000  5b 41 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |[A..............|
                 * 00000010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
                 * *
                 * 00000ff0  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 5d  |...............]|
                 * 00001000  5b 42 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |[B..............|
                 * 00001010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
                 * *
                 * 00002ff0  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 5d  |...............]|
                 * 00003000
                 */
                auto r = make_shared<reader>(std::move(f));
                return r->in.read_exactly(4096 + 8192).then([r] (temporary_buffer<char> buf) {
                    auto p = buf.get();
                    BOOST_REQUIRE(p[0] == '[' && p[1] == 'A' && p[4095] == ']');
                    BOOST_REQUIRE(p[4096] == '[' && p[4096 + 1] == 'B' && p[4096 + 8191] == ']');
                    return make_ready_future<>();
                }).then([r] {
                    return r->in.close();
                }).finally([r] {});
            }).finally([sem] () {
                sem->signal();
            });
        });

    return sem->wait();
}

SEASTAR_TEST_CASE(test_fstream_unaligned) {
    auto sem = make_lw_shared<semaphore>(0);

    open_file_dma("testfile.tmp",
                  open_flags::rw | open_flags::create | open_flags::truncate).then([sem] (file f) {
        auto w = make_shared<writer>(std::move(f));
        auto buf = static_cast<char*>(::malloc(40));
        memset(buf, 0, 40);
        buf[0] = '[';
        buf[1] = 'A';
        buf[39] = ']';
        w->out.write(buf, 40).then([buf, w] {
            ::free(buf);
            return w->out.close().then([w] {});
        }).then([] {
            return open_file_dma("testfile.tmp", open_flags::ro);
        }).then([] (file f) {
            return do_with(std::move(f), [] (file& f) {
                return f.size().then([] (size_t size) {
                    // assert that file was indeed truncated to the amount of bytes written.
                    BOOST_REQUIRE(size == 40);
                    return make_ready_future<>();
                });
            });
        }).then([] {
            return open_file_dma("testfile.tmp", open_flags::ro);
        }).then([] (file f) {
            auto r = make_shared<reader>(std::move(f));
            return r->in.read_exactly(40).then([r] (temporary_buffer<char> buf) {
                auto p = buf.get();
                BOOST_REQUIRE(p[0] == '[' && p[1] == 'A' && p[39] == ']');
                return make_ready_future<>();
            }).then([r] {
                return r->in.close();
            }).finally([r] {});
        }).finally([sem] () {
            sem->signal();
        });
    });

    return sem->wait();
}

future<> test_consume_until_end(uint64_t size) {
    return open_file_dma("testfile.tmp",
            open_flags::rw | open_flags::create | open_flags::truncate).then([size] (file f) {
            return do_with(make_file_output_stream(f), [size] (output_stream<char>& out) {
                std::vector<char> buf(size);
                std::iota(buf.begin(), buf.end(), 0);
                return out.write(buf.data(), buf.size()).then([&out] {
                   return out.flush();
                });
            }).then([f] {
                return f.size();
            }).then([size, f] (size_t real_size) {
                BOOST_REQUIRE_EQUAL(size, real_size);
            }).then([size, f] {
                auto consumer = [offset = uint64_t(0), size] (temporary_buffer<char> buf) mutable -> future<input_stream<char>::unconsumed_remainder> {
                    if (!buf) {
                        return make_ready_future<input_stream<char>::unconsumed_remainder>(temporary_buffer<char>());
                    }
                    BOOST_REQUIRE(offset + buf.size() <= size);
                    std::vector<char> expected(buf.size());
                    std::iota(expected.begin(), expected.end(), offset);
                    offset += buf.size();
                    BOOST_REQUIRE(std::equal(buf.begin(), buf.end(), expected.begin()));
                    return make_ready_future<input_stream<char>::unconsumed_remainder>(std::experimental::nullopt);
                };
                return do_with(make_file_input_stream(f), std::move(consumer), [size] (input_stream<char>& in, auto& consumer) {
                    return in.consume(consumer).then([&in] {
                        return in.close();
                    });
                });
            });
    });
}


SEASTAR_TEST_CASE(test_consume_aligned_file) {
    return test_consume_until_end(4096);
}

SEASTAR_TEST_CASE(test_consume_empty_file) {
    return test_consume_until_end(0);
}

SEASTAR_TEST_CASE(test_consume_unaligned_file) {
    return test_consume_until_end(1);
}

SEASTAR_TEST_CASE(test_consume_unaligned_file_large) {
    return test_consume_until_end((1 << 20) + 1);
}

SEASTAR_TEST_CASE(test_input_stream_esp_around_eof) {
    return seastar::async([] {
        auto flen = uint64_t(5341);
        auto rdist = std::uniform_int_distribution<char>();
        auto reng = std::default_random_engine();
        auto data = boost::copy_range<std::vector<uint8_t>>(
                boost::irange<uint64_t>(0, flen)
                | boost::adaptors::transformed([&] (int x) { return rdist(reng); }));
        auto f = open_file_dma("file.tmp",
                open_flags::rw | open_flags::create | open_flags::truncate).get0();
        auto out = make_file_output_stream(f);
        out.write(reinterpret_cast<const char*>(data.data()), data.size()).get();
        out.flush().get();
        //out.close().get();  // FIXME: closes underlying stream:?!
        struct range { uint64_t start; uint64_t end; };
        auto ranges = std::vector<range>{{
            range{0, flen},
            range{0, flen * 2},
            range{0, flen + 1},
            range{0, flen - 1},
            range{0, 1},
            range{1, 2},
            range{flen - 1, flen},
            range{flen - 1, flen + 1},
            range{flen, flen + 1},
            range{flen + 1, flen + 2},
            range{1023, flen-1},
            range{1023, flen},
            range{1023, flen + 2},
            range{8193, 8194},
            range{1023, 1025},
            range{1023, 1024},
            range{1024, 1025},
            range{1023, 4097},
        }};
        auto opt = file_input_stream_options();
        opt.buffer_size = 512;
        for (auto&& r : ranges) {
            auto start = r.start;
            auto end = r.end;
            auto len = end - start;
            auto in = make_file_input_stream(f, start, len, opt);
            std::vector<uint8_t> readback;
            auto more = true;
            while (more) {
                auto rdata = in.read().get0();
                for (size_t i = 0; i < rdata.size(); ++i) {
                    readback.push_back(rdata.get()[i]);
                }
                more = !rdata.empty();
            }
            //in.close().get();
            auto xlen = std::min(end, flen) - std::min(flen, start);
            BOOST_REQUIRE_EQUAL(xlen, readback.size());
            BOOST_REQUIRE(std::equal(readback.begin(), readback.end(), data.begin() + std::min(start, flen)));
        }
        f.close().get();
    });
}
