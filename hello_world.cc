/*
 * Copyright (C) 2016 ScyllaDB
 */

#include <iostream>
#include "core/app-template.hh"
#include "core/sleep.hh"

using namespace std::chrono_literals;

int main(int ac, char** av) {
    app_template app;

    return app.run(ac, av, [&app] {

        return sleep(1s).then([] {
            std::cout << "Hello World!\n";
            return make_ready_future<>(); 
        });
    });
}
