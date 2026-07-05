module;
#include <coroutine>

export module plexdb.test.log_consumer_helpers;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.pager;
import plexdb.pager.wal;
import plexdb.pager.types;
import plexdb.coroutine;

using namespace plexdb;

export {
    void register_test_log_consumer();
    void unregister_test_log_consumer();
}