module objstore.server;

import plexdb.base;

import objstore.tcp;
import objstore.engine;

using namespace plexdb;

namespace objstore::server {
    int get_http_status_for_execution_status(engine::ExecutionStatus status) {
        switch (status) {
            case engine::ExecutionStatus::Success:        return 200;
            case engine::ExecutionStatus::ServerError:    return 500;
            case engine::ExecutionStatus::SyntaxError:    return 400;
            case engine::ExecutionStatus::Unauthorized:   return 403;
            case engine::ExecutionStatus::Invalid:        return 400;
            case engine::ExecutionStatus::ConfigError:    return 400;
            case engine::ExecutionStatus::AlreadyExists:  return 409;
            case engine::ExecutionStatus::NotImplemented: return 501;
        }
        return 500;
    }
}
