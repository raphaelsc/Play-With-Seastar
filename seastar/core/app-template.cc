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

#include "app-template.hh"
#include "core/reactor.hh"
#include "core/scollectd.hh"
#include "core/print.hh"
#include <boost/program_options.hpp>
#include <boost/make_shared.hpp>
#include <fstream>
#include <cstdlib>

namespace bpo = boost::program_options;

app_template::app_template()
        : _opts("App options") {
    _opts.add_options()
            ("help,h", "show help message")
            ;
    _opts.add(reactor::get_options_description());
    _opts.add(smp::get_options_description());
    _opts.add(scollectd::get_options_description());
}

boost::program_options::options_description_easy_init
app_template::add_options() {
    return _opts.add_options();
}

void
app_template::add_positional_options(std::initializer_list<positional_option> options) {
    for (auto&& o : options) {
        _opts.add(boost::make_shared<bpo::option_description>(o.name, o.value_semantic, o.help));
        _pos_opts.add(o.name, o.max_count);
    }
}


bpo::variables_map&
app_template::configuration() {
    return *_configuration;
}

int
app_template::run(int ac, char ** av, std::function<future<int> ()>&& func) {
    return run_deprecated(ac, av, [func = std::move(func)] {
        func().then([] (int exit_code) {
            return engine().exit(exit_code);
        }).or_terminate();
    });
}

int
app_template::run(int ac, char ** av, std::function<future<> ()>&& func) {
    return run(ac, av, [func = std::move(func)] {
        return func().then([] () {
            return 0;
        });
    });
}

int
app_template::run_deprecated(int ac, char ** av, std::function<void ()>&& func) {
#ifdef DEBUG
    print("WARNING: debug mode. Not for benchmarking or production\n");
#endif
    bpo::variables_map configuration;
    try {
        bpo::store(bpo::command_line_parser(ac, av)
                    .options(_opts)
                    .positional(_pos_opts)
                    .run()
            , configuration);
        auto home = std::getenv("HOME");
        if (home) {
            std::ifstream ifs(std::string(home) + "/.config/seastar/seastar.conf");
            if (ifs) {
                bpo::store(bpo::parse_config_file(ifs, _opts), configuration);
            }
            std::ifstream ifs_io(std::string(home) + "/.config/seastar/io.conf");
            if (ifs_io) {
                bpo::store(bpo::parse_config_file(ifs_io, _opts), configuration);
            }
        }
    } catch (bpo::error& e) {
        print("error: %s\n\nTry --help.\n", e.what());
        return 2;
    }
    bpo::notify(configuration);
    if (configuration.count("help")) {
        std::cout << _opts << "\n";
        return 1;
    }
    configuration.emplace("argv0", boost::program_options::variable_value(std::string(av[0]), false));
    smp::configure(configuration);
    _configuration = {std::move(configuration)};
    engine().when_started().then([this] {
        scollectd::configure( this->configuration());
    }).then(
        std::move(func)
    ).then_wrapped([] (auto&& f) {
        try {
            f.get();
        } catch (std::exception& ex) {
            std::cout << "program failed with uncaught exception: " << ex.what() << "\n";
            engine().exit(1);
        }
    });
    auto exit_code = engine().run();
    smp::cleanup();
    return exit_code;
}
