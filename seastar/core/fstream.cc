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

#include "fstream.hh"
#include "align.hh"
#include "circular_buffer.hh"
#include "semaphore.hh"
#include "reactor.hh"
#include <malloc.h>
#include <string.h>

class file_data_source_impl : public data_source_impl {
    file _file;
    file_input_stream_options _options;
    uint64_t _pos;
    uint64_t _remain;
    circular_buffer<future<temporary_buffer<char>>> _read_buffers;
    unsigned _reads_in_progress = 0;
    std::experimental::optional<promise<>> _done;
public:
    file_data_source_impl(file f, uint64_t offset, uint64_t len, file_input_stream_options options)
            : _file(std::move(f)), _options(options), _pos(offset), _remain(len) {
        // prevent wraparounds
        _remain = std::min(std::numeric_limits<uint64_t>::max() - _pos, _remain);
    }
    virtual future<temporary_buffer<char>> get() override {
        if (_read_buffers.empty()) {
            issue_read_aheads(1);
        }
        auto ret = std::move(_read_buffers.front());
        _read_buffers.pop_front();
        return ret;
    }
    virtual future<> close() {
        _done.emplace();
        if (!_reads_in_progress) {
            _done->set_value();
        }
        return _done->get_future().then([this] {
            for (auto&& c : _read_buffers) {
                c.ignore_ready_future();
            }
        });
    }
private:
    void issue_read_aheads(unsigned min_ra = 0) {
        if (_done) {
            return;
        }
        auto ra = std::max(min_ra, _options.read_ahead);
        while (_read_buffers.size() < ra) {
            if (!_remain) {
                if (_read_buffers.size() >= min_ra) {
                    return;
                }
                _read_buffers.push_back(make_ready_future<temporary_buffer<char>>());
                continue;
            }
            ++_reads_in_progress;
            // if _pos is not dma-aligned, we'll get a short read.  Account for that.
            // Also avoid reading beyond _remain.
            uint64_t align = _file.disk_read_dma_alignment();
            auto start = align_down(_pos, align);
            auto end = align_up(std::min(start + _options.buffer_size, _pos + _remain), align);
            auto len = end - start;
            _read_buffers.push_back(_file.dma_read_bulk<char>(start, len, _options.io_priority_class).then_wrapped(
                    [this, start, end, pos = _pos, remain = _remain] (future<temporary_buffer<char>> ret) {
                issue_read_aheads();
                --_reads_in_progress;
                if (_done && !_reads_in_progress) {
                    _done->set_value();
                }
                if ((pos == start && end <= pos + remain) || ret.failed()) {
                    // no games needed
                    return ret;
                } else {
                    // first or last buffer, need trimming
                    auto tmp = ret.get0();
                    auto real_end = start + tmp.size();
                    if (real_end <= pos) {
                        return make_ready_future<temporary_buffer<char>>();
                    }
                    if (real_end > pos + remain) {
                        tmp.trim(pos + remain - start);
                    }
                    if (start < pos) {
                        tmp.trim_front(pos - start);
                    }
                    return make_ready_future<temporary_buffer<char>>(std::move(tmp));
                }
            }));
            auto old_pos = _pos;
            _pos = end;
            _remain = std::max(_pos, old_pos + _remain) - _pos;
        };
    }
};

class file_data_source : public data_source {
public:
    file_data_source(file f, uint64_t offset, uint64_t len, file_input_stream_options options)
        : data_source(std::make_unique<file_data_source_impl>(
                std::move(f), offset, len, options)) {}
};


input_stream<char> make_file_input_stream(
        file f, uint64_t offset, uint64_t len, file_input_stream_options options) {
    return input_stream<char>(file_data_source(std::move(f), offset, len, std::move(options)));
}

input_stream<char> make_file_input_stream(
        file f, uint64_t offset, file_input_stream_options options) {
    return make_file_input_stream(std::move(f), offset, std::numeric_limits<uint64_t>::max(), std::move(options));
}

input_stream<char> make_file_input_stream(
        file f, file_input_stream_options options) {
    return make_file_input_stream(std::move(f), 0, std::move(options));
}


class file_data_sink_impl : public data_sink_impl {
    file _file;
    file_output_stream_options _options;
    uint64_t _pos = 0;
    semaphore _write_behind_sem = { _options.write_behind };
    future<> _background_writes_done = make_ready_future<>();
    bool _failed = false;
public:
    file_data_sink_impl(file f, file_output_stream_options options)
            : _file(std::move(f)), _options(options) {}
    future<> put(net::packet data) { abort(); }
    virtual temporary_buffer<char> allocate_buffer(size_t size) override {
        return temporary_buffer<char>::aligned(_file.memory_dma_alignment(), size);
    }
    virtual future<> put(temporary_buffer<char> buf) override {
        uint64_t pos = _pos;
        _pos += buf.size();
        if (!_options.write_behind) {
            return do_put(pos, std::move(buf));
        }
        // Write behind strategy:
        //
        // 1. Issue N writes in parallel, using a semphore to limit to N
        // 2. Collect results in _background_writes_done, merging exception futures
        // 3. If we've already seen a failure, don't issue more writes.
        return _write_behind_sem.wait().then([this, pos, buf = std::move(buf)] () mutable {
            if (_failed) {
                _write_behind_sem.signal();
                auto ret = std::move(_background_writes_done);
                _background_writes_done = make_ready_future<>();
                return ret;
            }
            auto this_write_done = do_put(pos, std::move(buf)).finally([this] {
                _write_behind_sem.signal();
            });
            _background_writes_done = when_all(std::move(_background_writes_done), std::move(this_write_done))
                    .then([this] (std::tuple<future<>, future<>> possible_errors) {
                // merge the two errors, preferring the first
                auto& e1 = std::get<0>(possible_errors);
                auto& e2 = std::get<1>(possible_errors);
                if (e1.failed()) {
                    e2.ignore_ready_future();
                    return std::move(e1);
                } else {
                    if (e2.failed()) {
                        _failed = true;
                    }
                    return std::move(e2);
                }
            });
            return make_ready_future<>();
        });
    }
private:
    virtual future<> do_put(uint64_t pos, temporary_buffer<char> buf) {
        // put() must usually be of chunks multiple of file::dma_alignment.
        // Only the last part can have an unaligned length. If put() was
        // called again with an unaligned pos, we have a bug in the caller.
        assert(!(pos & (_file.disk_write_dma_alignment() - 1)));
        bool truncate = false;
        auto p = static_cast<const char*>(buf.get());
        size_t buf_size = buf.size();

        if ((buf.size() & (_file.disk_write_dma_alignment() - 1)) != 0) {
            // If buf size isn't aligned, copy its content into a new aligned buf.
            // This should only happen when the user calls output_stream::flush().
            auto tmp = allocate_buffer(align_up(buf.size(), _file.disk_write_dma_alignment()));
            ::memcpy(tmp.get_write(), buf.get(), buf.size());
            buf = std::move(tmp);
            p = buf.get();
            buf_size = buf.size();
            truncate = true;
        }

        return _file.dma_write(pos, p, buf_size, _options.io_priority_class).then(
                [this, buf = std::move(buf), truncate] (size_t size) {
            if (truncate) {
                return _file.truncate(_pos);
            }
            return make_ready_future<>();
        });
    }
    future<> wait() {
        return _write_behind_sem.wait(_options.write_behind).then([this] {
            return _background_writes_done.then([this] {
                // restore to pristine state; for flush() + close() sequence
                // (we allow either flush, or close, or both)
                _write_behind_sem.signal(_options.write_behind);
                _background_writes_done = make_ready_future<>();
            });
        });
    }
public:
    virtual future<> flush() override {
        return wait().then([this] {
            return _file.flush();
        });
    }
    virtual future<> close() {
        return wait().then([this] {
            return _file.close();
        });
    }
};

class file_data_sink : public data_sink {
public:
    file_data_sink(file f, file_output_stream_options options)
        : data_sink(std::make_unique<file_data_sink_impl>(
                std::move(f), options)) {}
};

output_stream<char> make_file_output_stream(file f, size_t buffer_size) {
    file_output_stream_options options;
    options.buffer_size = buffer_size;
    return make_file_output_stream(std::move(f), options);
}

output_stream<char> make_file_output_stream(file f, file_output_stream_options options) {
    return output_stream<char>(file_data_sink(std::move(f), options), options.buffer_size, true);
}

