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
 * Copyright (C) 2016 ScyllaDB
 *
 * The goal of this program is to allow a user to properly configure the Seastar I/O
 * scheduler.
 */
#include <chrono>
#include <random>
#include <memory>
#include <vector>
#include <cmath>
#include <libaio.h>
#include <wordexp.h>
#include <boost/thread/barrier.hpp>
#include <boost/filesystem.hpp>
#include <mutex>
#include <deque>
#include <queue>
#include <fstream>
#include "core/sstring.hh"
#include "core/posix.hh"
#include "core/reactor.hh"
#include "core/resource.hh"
#include "util/defer.hh"

using namespace std::chrono_literals;

bool filesystem_has_good_aio_support(sstring directory, bool verbose);

struct directory {
    sstring name;
    file_desc file;

    directory(sstring name) : name(name)
                            , file(file_desc::open(name.c_str(), O_DIRECTORY | O_CLOEXEC | O_RDONLY))
    {}
};

struct test_file {
    sstring name;
    file_desc file;

    test_file(const directory& dir) : name(dir.name + "/ioqueue-discovery")
                                    , file(file_desc::open(name.c_str(),  O_DIRECT | O_CLOEXEC | O_RDWR | O_CREAT, S_IRWXU))
    {
        ::unlink(name.c_str());
    }
    void generate();
};

struct run_stats {
    uint64_t IOPS;
    uint64_t concurrency;
    run_stats(uint64_t iops = 0, uint64_t conc = 0) : IOPS(iops), concurrency(conc) {}
    run_stats& operator+=(const struct run_stats& stats) {
        if (stats.concurrency != 0) {
            IOPS += stats.IOPS;
            concurrency += stats.concurrency;
        }
        return *this;
    }
};

class iotune_manager {
public:
    enum class test_done { no, yes };
    using clock = std::chrono::steady_clock;
    static constexpr uint64_t file_size = 10ull << 30;
    static constexpr uint64_t wbuffer_size = 128ul << 10;
    static constexpr uint64_t rbuffer_size = 4ul << 10;
private:
    // We need all threads to synchronize and start the various phases at the same time.
    // Each of them will serve a purpose:
    //
    // A thread can only start running before the previous result is found.
    // Before that, we don't know which concurrency level to test next. So
    // before starting, everybody waits for _start_run_barrier
    boost::barrier _start_run_barrier;
    // All threads must finish before we can compute results. Therefore, after finishing,
    // they all must gather around _finish_run_barrier
    boost::barrier _finish_run_barrier;
    // After computing the results, the coordinator thread will decide whether
    // or not to proceed with another round. All threads synchronize again at
    // this point to wait for that decision.
    boost::barrier _results_barrier;

    // We need a synchronization point once more, when deciding at which time
    // to start counting requests. We will avoid a barrier here due to its
    // sleeping nature. We would have to give a lot more leeway if using a barrier,
    // to account for the wake up times.
    //
    // The coordinator thread will decide on the start time, while all the others
    // just busy wait for that decision.
    std::atomic<unsigned> _time_run_atomic;

    test_done _test_done = { test_done::no };

    test_file _test_file;

    // The test result for the current iteration is stored at this structure.
    // We'll protect it with a mutex, so when a thread is finished it can just
    // add its information here.
    run_stats _test_result;
    std::mutex _result_mutex;
    std::vector<std::thread> _threads;

    iotune_manager::clock::time_point _run_start_time;

    run_stats issue_reads(size_t cpu_id, unsigned this_concurrency);

    run_stats current_result(size_t cpu_id) {
        assert(cpu_id == 0);
        std::lock_guard<std::mutex> guard(_result_mutex);
        return _test_result;
    }

    // Test management
    //
    // Those are the various knobs used to control the execution of the
    // evaluation.
    //
    // We will keep track of the best result we have so far. This is used
    // to find the maximum seen throughput.
    run_stats _best_result;
    // For how long to run the test for. As we progress with the estimation,
    // we will increase the time of each run.
    clock::duration _phase_timing = 250ms;
    // Next concurrency to test. This will be always pop'd from the cuncurrency
    // queue, but stored in a normal integer for convenience of the helper threads.
    unsigned _next_concurrency = 4;
    // Queue of pending concurrency tests.
    std::queue<unsigned> _concurrency_queue;
    // For the last phase of the test, we want to find the concurrency that puts
    // us closer to _desired_percentile * IOPS_max. We store what we have found
    // so far here.
    float _desired_percentile = 0.8;
    uint64_t _best_critical_concurrency = 0;
    uint64_t _best_critical_delta = std::numeric_limits<uint64_t>::max();

    // Test phases
    //
    // Phase1: In which region lies the maximum throughput?
    //
    // We will run each concurrency level for not very long. We don't really care if the number
    // of IOPS we find is not totally high precision, and we'll run the region in large steps.
    //
    // There are heuristics that could be used to try stopping early, but we will just go very
    // far into a high concurrency number. Each iteration is, after all, relatively cheap.
    //
    // Phase2: What is the maximum throughput?
    //
    // We assume that the maximum throughput lies somewhere around the region where we found
    // the current maximum. So we test the range around it, one by one - including the current
    // maximum. This is because we will now run each test for longer, so we'll hopefuly find a
    // better estimate.
    //
    // Phase 3: What is the first concurrency level that yields a certain percentile of the
    // maximum throughput?
    //
    // We will now scan a region that is around the desired percentile, and we will run this
    // for the longest. Somewhere in that region, has to lie a concurrency level that puts us
    // at the desired percentile.
    enum class test_phase { find_max_region, find_max_point, find_percentile };
    test_phase _current_test_phase = test_phase::find_max_region;

    // We have to have a zero-point here, otherwise the search may fail if the disk
    // saturates very rapidly.
    //
    // concurrency, IOPS. We could use a set here and specialize the less comparator,
    // but sometimes we need to compare results based on IOPS, sometimes based on
    // concurrency.
    std::map<uint64_t, uint64_t> _all_results = { {0ul, 0ul} };

    void find_max_region(const run_stats& result) {
        if (result.IOPS > _best_result.IOPS) {
            _best_result = result;
        }
        // Now try to explore the region around the maximum to see
        // if we find anything higher than the current seen maximum
        if (_concurrency_queue.empty()) {
            std::cout << "Refining search for maximum. So far, " << _best_result.IOPS <<  " IOPS" << std::endl;
            _phase_timing = 500ms;
            auto it = _all_results.find(_best_result.concurrency);
            auto prev = std::prev(it);
            auto next = std::next(it);

            for (auto i = (*prev).first; i < (*next).first; ++i) {
                _concurrency_queue.push(i);
            }
            _current_test_phase = test_phase::find_max_point;
        }
    }

    void find_max_point(const run_stats& result, float percentile) {
        if (result.IOPS > _best_result.IOPS) {
            _best_result = result;
        }
        if (_concurrency_queue.empty()) {
            std::cout << "Maximum throughput: " << _best_result.IOPS <<  " IOPS" << std::endl;
            _phase_timing = 2000ms;
            _current_test_phase = test_phase::find_percentile;

            std::map<uint64_t, uint64_t>::iterator iterator_of_minimum = _all_results.begin();
            std::map<uint64_t, uint64_t>::iterator iterator_of_maximum = _all_results.end();
            for (auto it = _all_results.begin(); it != _all_results.end(); ++it) {
                if (((*it).second > (percentile - 0.20) * _best_result.IOPS) && (iterator_of_minimum == _all_results.begin())) {
                    iterator_of_minimum = it;
                } else if ((*it).second > ((percentile + 0.10) * _best_result.IOPS)) {
                    iterator_of_maximum = it;
                    break;
                }
            }

            for (auto i = (*iterator_of_minimum).first; i < (*iterator_of_maximum).first; ++i) {
                _concurrency_queue.push(i);
            }
        }
    }

    void find_next_concurrency(const run_stats& result) {
        if (_current_test_phase == test_phase::find_max_region) {
            find_max_region(result);
        } else if (_current_test_phase == test_phase::find_max_point) {
            find_max_point(result, _desired_percentile);
        } else {
            uint64_t critical_IOPS = _desired_percentile * _best_result.IOPS;
            uint64_t d = std::abs(int64_t(critical_IOPS - result.IOPS));
            if (d < _best_critical_delta) {
                _best_critical_delta = d;
                _best_critical_concurrency = result.concurrency;
            }
        }
    }
public:
    iotune_manager(size_t n, sstring dirname) : _start_run_barrier(n)
                                              , _finish_run_barrier(n)
                                              , _results_barrier(n)
                                              , _time_run_atomic(n)
                                              , _test_file(directory(dirname))
                                              , _run_start_time(iotune_manager::clock::now()) {
        _test_file.generate();
        // Initial exploratory run
        for (auto initial: boost::irange<unsigned, unsigned>(4, 512, 4)) {
            _concurrency_queue.push(initial);
        }
    }
    template <typename Func>
    void spawn_new(Func&& func) {
        _threads.emplace_back(std::thread(std::forward<Func>(func)));
    }

    clock::time_point get_start_time(size_t cpu_id) {
        if (cpu_id == 0) {
            std::lock_guard<std::mutex> guard(_result_mutex);
            _test_result = run_stats();
            _run_start_time = iotune_manager::clock::now() + 100ms;
        }
        _time_run_atomic.fetch_sub(1, std::memory_order_release);
        while (_time_run_atomic.load(std::memory_order_acquire) != 0);
        return _run_start_time;
    }

    clock::duration current_total_time() const {
        return _phase_timing;
    }

    unsigned get_thread_concurrency(size_t cpu_id) {
        _start_run_barrier.wait();
        auto overall_concurrency = _next_concurrency;
        auto my_concurrency = overall_concurrency / _threads.size();
        if (cpu_id < (overall_concurrency % _threads.size())) {
            my_concurrency++;
        }
        return my_concurrency;
    }

    void run_test(size_t cpu_id, unsigned concurrency) {
        if (concurrency != 0) {
            auto r = issue_reads(cpu_id, concurrency);
            std::lock_guard<std::mutex> guard(_result_mutex);
            _test_result += r;
        } else {
            // We won't run, but we need to signal to the other threads
            // that we are ready so they don't keep waiting.
            _time_run_atomic.fetch_sub(1, std::memory_order_release);
        }
        _finish_run_barrier.wait();
    }

    test_done analyze_results(size_t cpu_id) {
        if (cpu_id == 0) {
            struct run_stats result = current_result(cpu_id);
            _all_results[result.concurrency] = result.IOPS;
            find_next_concurrency(result);
            // Still empty, nothing else to do.
            if (_concurrency_queue.empty()) {
                _test_done = test_done::yes;
            } else {
                _next_concurrency = _concurrency_queue.front();
                _concurrency_queue.pop();
            }
        }
        _time_run_atomic.fetch_add(1, std::memory_order_release);
        _results_barrier.wait();
        return _test_done;
    }

    uint32_t finish_estimate() {
        for (auto&& t: _threads) {
            t.join();
        }

        // We now have a point where the curve starts to bend, which means,
        // latency is increasing while throughput is not. We, however, don't
        // want to put Seastar's I/O queue at exactly this point. We have all
        // sorts of delays throughout the stack, including in the Linux kernel.
        //
        // Moreover, not all disks have a beautiful, well behaved, and monotonic graph.
        //
        // Empirically, we will just allow three times as much as the number we have found.
        return _best_critical_concurrency * 3;
    }
};

constexpr uint64_t iotune_manager::file_size;
constexpr uint64_t iotune_manager::wbuffer_size;
constexpr uint64_t iotune_manager::rbuffer_size;

static thread_local std::default_random_engine random_generator(std::chrono::duration_cast<std::chrono::nanoseconds>(iotune_manager::clock::now().time_since_epoch()).count());

class reader {
    uint64_t _opcount = 0;
    file_desc _file;
    std::uniform_int_distribution<uint32_t> _pos_distribution;
    struct iocb _iocb;
    iotune_manager::clock::time_point _start_time;
    iotune_manager::clock::time_point _tstamp;
    iotune_manager::clock::time_point _end_time;
    std::unique_ptr<char[], free_deleter> _buf;
public:
    reader(file_desc f, iotune_manager::clock::time_point start_time, iotune_manager::clock::time_point end_time)
                : _file(std::move(f))
                , _pos_distribution(0, (iotune_manager::file_size/ iotune_manager::rbuffer_size) - 1)
                , _start_time(start_time)
                , _tstamp(iotune_manager::clock::now())
                , _end_time(end_time)
                , _buf(allocate_aligned_buffer<char>(iotune_manager::rbuffer_size, 4096))
    {}

    iocb* issue() {
        io_prep_pread(&_iocb, _file.get(), _buf.get(), iotune_manager::rbuffer_size, _pos_distribution(random_generator) * iotune_manager::rbuffer_size);
        _iocb.data = this;
        _tstamp = std::chrono::steady_clock::now();
        return &_iocb;
    }

    iocb* req_finished() {
        auto now = std::chrono::steady_clock::now();
        if ((now > _start_time) && (now < _end_time)) {
            ++_opcount;
        }

        if (now < _end_time) {
            return issue();
        }

        return nullptr;
    }

    struct run_stats get_stats() {
        float IOPS = _opcount / std::chrono::duration_cast<std::chrono::duration<double>>(_end_time - _start_time).count();
        return { uint64_t(IOPS), 1 };
    }
};

void sanity_check_ev(const io_event& ev, size_t size) {
    throw_kernel_error(long(ev.res));
    if (size_t(ev.res) != size) {
        throw std::runtime_error(sprint("Expected %ld bytes I/O, found %ld\n", size, size_t(ev.res)));
    }
}

run_stats iotune_manager::issue_reads(size_t cpu_id, unsigned concurrency) {
    io_context_t io_context = {0};
    auto r = ::io_setup(concurrency, &io_context);
    assert(r >= 0);
    auto destroyer = defer([&io_context] { ::io_destroy(io_context); });

    unsigned finished = 0;
    std::vector<io_event> ev;
    ev.resize(concurrency);

    std::vector<iocb*> iocb_vecptr;
    iocb_vecptr.reserve(concurrency);

    auto start_time = get_start_time(cpu_id);
    auto total_time = current_total_time();

    auto fds = std::vector<reader>();
    for (unsigned i = 0u; i < concurrency; ++i) {
        fds.emplace_back(_test_file.file.dup(), start_time, start_time + total_time);
    }

    for (auto& r: fds) {
        iocb_vecptr.push_back(r.issue());
    }

    r = ::io_submit(io_context, iocb_vecptr.size(), iocb_vecptr.data());
    throw_kernel_error(r);

    struct timespec timeout = {0, 0};
    while (finished != concurrency) {
        int n = ::io_getevents(io_context, 1, ev.size(), ev.data(), &timeout);
        throw_kernel_error(n);
        unsigned new_req = 0;
        for (auto i = 0ul; i < size_t(n); ++i) {
            sanity_check_ev(ev[i], iotune_manager::rbuffer_size);
            auto reader_ptr = reinterpret_cast<reader*>(ev[i].data);
            auto iocb_ptr = reader_ptr->req_finished();
            if (iocb_ptr == nullptr) {
                finished++;
            } else {
                iocb_vecptr[new_req++] = iocb_ptr;
            }
        }
        r = ::io_submit(io_context, new_req, iocb_vecptr.data());
        throw_kernel_error(r);
    }
    struct run_stats result;
    for (auto&& r: fds) {
        result += r.get_stats();
    }
    return result;
}

void test_file::generate() {
    std::cout << "Generating evaluation file..." << std::flush;
    io_context_t io_context = {0};
    auto max_aio = 128;
    auto r = ::io_setup(max_aio, &io_context);
    assert(r >= 0);
    auto destroyer = defer([&io_context] { ::io_destroy(io_context); });

    auto buf = allocate_aligned_buffer<char>(iotune_manager::wbuffer_size, 4096);
    memset(buf.get(), 0, iotune_manager::wbuffer_size);
    auto ft = ftruncate(file.get(), iotune_manager::file_size);
    throw_kernel_error(ft);

    std::vector<iocb*> iocb_vecptr;
    std::vector<iocb> iocbs;
    std::vector<io_event> ev;

    iocbs.resize(max_aio);
    ev.resize(max_aio);
    iocb_vecptr.resize(max_aio);
    std::iota(iocb_vecptr.begin(), iocb_vecptr.end(), iocbs.data());
    uint64_t pos = 0;
    unsigned aio_outstanding = 0;

    while (pos < iotune_manager::file_size || aio_outstanding) {
        unsigned i = 0;
        while (i < max_aio - aio_outstanding && pos < iotune_manager::file_size) {
            auto now = std::min(iotune_manager::file_size - pos, iotune_manager::wbuffer_size);
            auto& iocb = iocbs[i++];
            iocb.data = buf.get();
            io_prep_pwrite(&iocb, file.get(), buf.get(), now, pos);
            pos += now;
        }
        if (i) {
            r = ::io_submit(io_context, i, iocb_vecptr.data());
            throw_kernel_error(r);
            aio_outstanding += r;
        }
        if (aio_outstanding) {
            struct timespec timeout = {0, 0};
            int n = ::io_getevents(io_context, 1, ev.size(), ev.data(), &timeout);
            throw_kernel_error(n);
            for (auto i = 0ul; i < size_t(n); ++i) {
                sanity_check_ev(ev[i], iotune_manager::wbuffer_size);
            }
            aio_outstanding -= n;
        }
    }
    std::cout << " done." << std::endl;
}

uint32_t io_queue_discovery(sstring dir, std::vector<unsigned> cpus) {
    iotune_manager iotune_manager(cpus.size(), dir);

    for (auto i = 0ul; i < cpus.size(); ++i) {
        iotune_manager.spawn_new([&cpus, &iotune_manager, id = i] {
            pin_this_thread(cpus[id]);
            do {
                auto my_concurrency = iotune_manager.get_thread_concurrency(id);
                iotune_manager.run_test(id, my_concurrency);
            } while (iotune_manager.analyze_results(id) == iotune_manager::test_done::no);
        });
    }

    return iotune_manager.finish_estimate();
}

void write_configuration_file(std::string conf_file, std::string format, unsigned max_io_requests, std::experimental::optional<unsigned> num_io_queues = {}) {
    std::cout << "Recommended --max-io-requests: " << max_io_requests << std::endl;
    if (num_io_queues) {
        std::cout << "Recommended --num-io-queues: " << *num_io_queues << std::endl;
    }

    wordexp_t k;
    // Do tilde expansion if needed, but since we get the directory from the user, it
    // can be anything. So just rely on posix for that.
    wordexp(conf_file.c_str(), &k, 0);
    assert(k.we_wordc == 1);
    boost::filesystem::path conf_path(k.we_wordv[0]);
    wordfree(&k);

    // No need to test the return value here. If by any chance the directory was not created,
    // writing the file itself will generate an exception.
    boost::filesystem::create_directories(conf_path.parent_path());
    try {
        std::ofstream ofs_io;
        ofs_io.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        ofs_io.open(conf_path.string(), std::ofstream::trunc);
        if (ofs_io) {
            if (format == "seastar") {
                ofs_io << "max-io-requests=" << max_io_requests << std::endl;
                if (num_io_queues) {
                    ofs_io << "num-io-queues=" << *num_io_queues << std::endl;
                }
            } else {
                ofs_io << "SEASTAR_IO=\"--max-io-requests=" << max_io_requests;
                if (num_io_queues) {
                    ofs_io << " --num-io-queues=" << *num_io_queues;
                }
                ofs_io << "\"" << std::endl;
            }
        }
        ofs_io.close();
        std::cout << "Written the above values to " << conf_path.string() << std::endl;
    } catch (std::ios_base::failure& e) {
        std::cout << e.what() << " when writing configuration file. Please add them to your seastar command line" << std::endl;
    }
}

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    bpo::options_description desc("Parameters for evaluation. This is intended to be ran with parameters that will match the desired use.");
    desc.add_options()
        ("help,h", "show help message")
        ("evaluation-directory", bpo::value<sstring>()->required(), "directory where to execute the evaluation")
        ("cpuset", bpo::value<cpuset_bpo_wrapper>(), "CPUs to use (in cpuset(7) format; default: all))")
        ("options-file", bpo::value<sstring>()->default_value("~/.config/seastar/io.conf"), "Output configuration file")
        ("format", bpo::value<sstring>()->default_value("seastar"), "Configuration file format (seastar | envfile)")
    ;

    bpo::variables_map configuration;
    try {
        bpo::store(bpo::parse_command_line(ac, av, desc), configuration);
    } catch (bpo::error& e) {
        print("error: %s\n\nTry --help.\n", e.what());
        return 2;
    }
    if (configuration.count("help")) {
        std::cout << desc << "\n";
        return 1;
    }
    auto format = configuration["format"].as<sstring>();
    if (format != "seastar" && format != "envfile") {
        std::cout << desc << "\n";
        return 1;
    }
    bpo::notify(configuration);

    auto conf_file = configuration["options-file"].as<sstring>();

    std::vector<unsigned> cpuvec;
    sstring directory;
    if (configuration.count("cpuset")) {
        for (auto& c: configuration["cpuset"].as<cpuset_bpo_wrapper>().value) {
            cpuvec.push_back(c);
        }
    } else {
        for (auto c = 0u; c < resource::nr_processing_units(); ++c) {
            cpuvec.push_back(c);
        }
    }

    directory = configuration["evaluation-directory"].as<sstring>();

    if (!filesystem_has_good_aio_support(directory, false)) {
        std::cerr << "File system on " << directory << " is not qualified for seastar AIO;"
                " see http://www.scylladb.com/kb/kb-fs-not-qualified-aio/ for details\n";
        return 1;
    }

    auto iodepth = io_queue_discovery(directory, cpuvec);
    auto num_io_queues = cpuvec.size();
    if (iodepth / num_io_queues < 4) {
        num_io_queues = iodepth / 4;
    }

    if (num_io_queues != cpuvec.size()) {
        iodepth = (iodepth / num_io_queues) * num_io_queues;
        write_configuration_file(conf_file, format, iodepth, num_io_queues);
    } else {
        write_configuration_file(conf_file, format, iodepth);
    }
    return 0;
}
