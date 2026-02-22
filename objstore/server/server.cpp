module objstore.server;

import plexdb.base;

import objstore.tcp;
import objstore.engine;

using namespace plexdb;

namespace objstore::server {
    void return_execution_result(tcp::Request& req, engine::ExecutionResult result, const String8& value) {
        switch (result) {
            case engine::ExecutionResult::BadRequest:{
                tcp::return_http_fail(req, 400, value);
            }break;
            case engine::ExecutionResult::NotImplemented:{
                tcp::return_http_fail(req, 501, value);
            }
            case engine::ExecutionResult::Success:{
                tcp::return_http_success(req, value);
            }
        }
    }
}
