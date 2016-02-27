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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifndef NET_NATIVE_STACK_IMPL_HH_
#define NET_NATIVE_STACK_IMPL_HH_

#include "core/reactor.hh"
#include "stack.hh"

namespace net {

template <typename Protocol>
class native_server_socket_impl;

template <typename Protocol>
class native_connected_socket_impl;

class native_network_stack;

// native_server_socket_impl
template <typename Protocol>
class native_server_socket_impl : public server_socket_impl {
    typename Protocol::listener _listener;
public:
    native_server_socket_impl(Protocol& proto, uint16_t port, listen_options opt);
    virtual future<connected_socket, socket_address> accept() override;
    virtual void abort_accept() override;
};

template <typename Protocol>
native_server_socket_impl<Protocol>::native_server_socket_impl(Protocol& proto, uint16_t port, listen_options opt)
    : _listener(proto.listen(port)) {
}

template <typename Protocol>
future<connected_socket, socket_address>
native_server_socket_impl<Protocol>::accept() {
    return _listener.accept().then([this] (typename Protocol::connection conn) {
        return make_ready_future<connected_socket, socket_address>(
                connected_socket(std::make_unique<native_connected_socket_impl<Protocol>>(std::move(conn))),
                socket_address()); // FIXME: don't fake it
    });
}

template <typename Protocol>
void
native_server_socket_impl<Protocol>::abort_accept() {
    _listener.abort_accept();
}

// native_connected_socket_impl
template <typename Protocol>
class native_connected_socket_impl : public connected_socket_impl {
    typename Protocol::connection _conn;
    class native_data_source_impl;
    class native_data_sink_impl;
public:
    explicit native_connected_socket_impl(typename Protocol::connection conn)
        : _conn(std::move(conn)) {}
    virtual data_source source() override;
    virtual data_sink sink() override;
    virtual future<> shutdown_input() override;
    virtual future<> shutdown_output() override;
    virtual void set_nodelay(bool nodelay) override;
    virtual bool get_nodelay() const override;
};

template <typename Protocol>
class native_connected_socket_impl<Protocol>::native_data_source_impl final
    : public data_source_impl {
    typename Protocol::connection& _conn;
    size_t _cur_frag = 0;
    bool _eof = false;
    packet _buf;
public:
    explicit native_data_source_impl(typename Protocol::connection& conn)
        : _conn(conn) {}
    virtual future<temporary_buffer<char>> get() override {
        if (_eof) {
            return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>(0));
        }
        if (_cur_frag != _buf.nr_frags()) {
            auto& f = _buf.fragments()[_cur_frag++];
            return make_ready_future<temporary_buffer<char>>(
                    temporary_buffer<char>(f.base, f.size,
                            make_deleter(deleter(), [p = _buf.share()] () mutable {})));
        }
        return _conn.wait_for_data().then([this] {
            _buf = _conn.read();
            _cur_frag = 0;
            _eof = !_buf.len();
            return get();
        });
    }
};

template <typename Protocol>
class native_connected_socket_impl<Protocol>::native_data_sink_impl final
    : public data_sink_impl {
    typename Protocol::connection& _conn;
public:
    explicit native_data_sink_impl(typename Protocol::connection& conn)
        : _conn(conn) {}
    virtual future<> put(packet p) override {
        return _conn.send(std::move(p));
    }
    virtual future<> close() override {
        _conn.close_write();
        return make_ready_future<>();
    }
};

template <typename Protocol>
data_source native_connected_socket_impl<Protocol>::source() {
    return data_source(std::make_unique<native_data_source_impl>(_conn));
}

template <typename Protocol>
data_sink native_connected_socket_impl<Protocol>::sink() {
    return data_sink(std::make_unique<native_data_sink_impl>(_conn));
}

template <typename Protocol>
future<>
native_connected_socket_impl<Protocol>::shutdown_input() {
    _conn.close_read();
    return make_ready_future<>();
}

template <typename Protocol>
future<>
native_connected_socket_impl<Protocol>::shutdown_output() {
    _conn.close_write();
    return make_ready_future<>();
}

template <typename Protocol>
void
native_connected_socket_impl<Protocol>::set_nodelay(bool nodelay) {
    // FIXME: implement
}

template <typename Protocol>
bool
native_connected_socket_impl<Protocol>::get_nodelay() const {
    // FIXME: implement
    return true;
}

}



#endif /* NET_NATIVE_STACK_IMPL_HH_ */
