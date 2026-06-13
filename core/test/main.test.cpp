#include <catch2/catch_session.hpp>

import plexdb.threads;

int main(int argc, char* argv[]) {
    plexdb::threads::Context main_thread_ctx{.arenas = {}, .is_main = true};
    plexdb::threads::equip(&main_thread_ctx);

    return Catch::Session().run(argc, argv);
}