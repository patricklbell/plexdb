#include <catch2/catch_session.hpp>

import plexdb.threads;

int main( int argc, char* argv[] ) {
    plexdb::ThreadContext main_thread_ctx{};
    plexdb::threads::equip(&main_thread_ctx);

    return Catch::Session().run( argc, argv );
}