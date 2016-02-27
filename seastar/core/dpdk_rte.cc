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
#ifdef HAVE_DPDK

#include "net/dpdk.hh"
#include "core/dpdk_rte.hh"
#include "util/conversions.hh"
#include <experimental/optional>
#include <rte_pci.h>

namespace dpdk {

bool eal::initialized = false;

void eal::init(cpuset cpus, boost::program_options::variables_map opts)
{
    if (initialized) {
        return;
    }

    std::stringstream mask;
    mask << std::hex << cpus.to_ulong();

    // TODO: Inherit these from the app parameters - "opts"
    std::vector<std::vector<char>> args {
        string2vector(opts["argv0"].as<std::string>()),
        string2vector("-c"), string2vector(mask.str()),
        string2vector("-n"), string2vector("1")
    };

    std::experimental::optional<std::string> hugepages_path;
    if (opts.count("hugepages")) {
        hugepages_path = opts["hugepages"].as<std::string>();
    }

    // If "hugepages" is not provided and DPDK PMD drivers mode is requested -
    // use the default DPDK huge tables configuration.
    if (hugepages_path) {
        args.push_back(string2vector("--huge-dir"));
        args.push_back(string2vector(hugepages_path.value()));

        //
        // We don't know what is going to be our networking configuration so we
        // assume there is going to be a queue per-CPU. Plus we'll give a DPDK
        // 64MB for "other stuff".
        //
        size_t size_MB = mem_size(cpus.count()) >> 20;
        std::stringstream size_MB_str;
        size_MB_str << size_MB;

        args.push_back(string2vector("-m"));
        args.push_back(string2vector(size_MB_str.str()));
    } else if (!opts.count("dpdk-pmd")) {
        args.push_back(string2vector("--no-huge"));
    }
#ifdef HAVE_OSV
    args.push_back(string2vector("--no-shconf"));
#endif

    std::vector<char*> cargs;

    for (auto&& a: args) {
        cargs.push_back(a.data());
    }
    /* initialise the EAL for all */
    int ret = rte_eal_init(cargs.size(), cargs.data());
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot init EAL\n");
    }

    initialized = true;
}

uint32_t __attribute__((weak)) qp_mempool_obj_size(bool hugetlbfs_membackend)
{
    return 0;
}

size_t eal::mem_size(int num_cpus, bool hugetlbfs_membackend)
{
    size_t memsize = 0;
    //
    // PMD mempool memory:
    //
    // We don't know what is going to be our networking configuration so we
    // assume there is going to be a queue per-CPU.
    //
    memsize += num_cpus * qp_mempool_obj_size(hugetlbfs_membackend);

    // Plus we'll give a DPDK 64MB for "other stuff".
    memsize += (64UL << 20);

    return memsize;
}

} // namespace dpdk

#endif // HAVE_DPDK
