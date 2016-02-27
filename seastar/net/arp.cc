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

#include "arp.hh"

namespace net {

arp_for_protocol::arp_for_protocol(arp& a, uint16_t proto_num)
    : _arp(a), _proto_num(proto_num) {
    _arp.add(proto_num, this);
}

arp_for_protocol::~arp_for_protocol() {
    _arp.del(_proto_num);
}

arp::arp(interface* netif) : _netif(netif), _proto(netif, eth_protocol_num::arp, [this] { return get_packet(); })
    , _rx_packets(_proto.receive([this] (packet p, ethernet_address ea) {
        return process_packet(std::move(p), ea);
    },
    [this](forward_hash& out_hash_data, packet& p, size_t off) {
        return forward(out_hash_data, p, off);
    })) {
}

std::experimental::optional<l3_protocol::l3packet> arp::get_packet() {
    std::experimental::optional<l3_protocol::l3packet> p;
    if (!_packetq.empty()) {
        p = std::move(_packetq.front());
        _packetq.pop_front();
    }
    return p;
}

bool arp::forward(forward_hash& out_hash_data, packet& p, size_t off) {
    auto ah = p.get_header<arp_hdr>(off);
    auto i = _arp_for_protocol.find(ntoh(ah->ptype));
    if (i != _arp_for_protocol.end()) {
        return i->second->forward(out_hash_data, p, off);
    }
    return false;
}

void arp::add(uint16_t proto_num, arp_for_protocol* afp) {
    _arp_for_protocol[proto_num] = afp;
}

void arp::del(uint16_t proto_num) {
    _arp_for_protocol.erase(proto_num);
}

future<>
arp::process_packet(packet p, ethernet_address from) {
    auto ah = ntoh(*p.get_header<arp_hdr>());
    auto i = _arp_for_protocol.find(ah.ptype);
    if (i != _arp_for_protocol.end()) {
        i->second->received(std::move(p));
    }
    return make_ready_future<>();
}

}
