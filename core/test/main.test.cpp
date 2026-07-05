#include <catch2/catch_session.hpp>
#include <catch2/catch_config.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <stdio.h>

import plexdb.threads;
import plexdb.base;
import plexdb.test.log_consumer_helpers;

int main(int argc, char* argv[]) {
    plexdb::threads::Context main_thread_ctx{.arenas = {}, .is_main = true};
    plexdb::threads::equip(&main_thread_ctx);

    using namespace Catch::Clara;
    Catch::Session session;

    bool redirect_plugin = false;
    {
        auto cli = session.cli() | Opt(redirect_plugin)["--redirect-plugin"]("Redirect plugin output to info");
        session.cli(cli);
        int code = session.applyCommandLine(argc, argv);
        if (code != 0) {
            return code;
        }
    }

    if (redirect_plugin) {
        register_test_log_consumer();
    }

    int code = session.run();

    if (redirect_plugin) {
        unregister_test_log_consumer();
    }
    return code;
}