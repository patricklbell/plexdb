#include <catch2/catch_test_macros.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

import plexdb.base;
import plexdb.os;

using namespace plexdb;
using namespace plexdb::os;

TEST_CASE("signal_pipe_create returns valid fds", "[plexdb.os.signal]") {
    SignalPipe pipe = signal_pipe_create();
    REQUIRE(pipe.read_fd >= 0);
    REQUIRE(pipe.write_fd >= 0);
    REQUIRE(pipe.read_fd != pipe.write_fd);
    signal_pipe_destroy(pipe);
}

TEST_CASE("signal_pipe_destroy closes fds", "[plexdb.os.signal]") {
    SignalPipe pipe = signal_pipe_create();
    int rd = pipe.read_fd;
    int wr = pipe.write_fd;

    signal_pipe_destroy(pipe);
    REQUIRE(pipe.read_fd == -1);
    REQUIRE(pipe.write_fd == -1);

    // fds should be closed - fcntl on closed fd returns -1
    REQUIRE(fcntl(rd, F_GETFD) == -1);
    REQUIRE(fcntl(wr, F_GETFD) == -1);
}

TEST_CASE("signal_pipe_notify makes read_fd readable", "[plexdb.os.signal]") {
    SignalPipe pipe = signal_pipe_create();

    // set read_fd to non-blocking
    int flags = fcntl(pipe.read_fd, F_GETFL, 0);
    fcntl(pipe.read_fd, F_SETFL, flags | O_NONBLOCK);

    // before notify, read should fail
    char buf;
    REQUIRE(read(pipe.read_fd, &buf, 1) == -1);

    signal_pipe_notify(pipe);

    // after notify, read should succeed
    REQUIRE(read(pipe.read_fd, &buf, 1) == 1);

    signal_pipe_destroy(pipe);
}

static volatile int g_test_signal_received = 0;
static void test_signal_handler(int) {
    g_test_signal_received = 1;
}

TEST_CASE("signal_register installs handler", "[plexdb.os.signal]") {
    g_test_signal_received = 0;

    signal_register(SIGUSR2, test_signal_handler);
    raise(SIGUSR2);

    REQUIRE(g_test_signal_received == 1);

    // restore default
    signal(SIGUSR2, SIG_DFL);
}
