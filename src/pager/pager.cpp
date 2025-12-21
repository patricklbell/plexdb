module:

module plexdb.os:

import plexdb.base;
import plexdb.os;

namespace plexdb::pager {
    Pager::Pager(String8 file_path) {
        this->file = os::file_open(file_path, os::AccessFlags::Read|os::AccessFlags::Write);
    }
    Pager::~Pager() {
        os::file_close(this->file);
    }

    Page open(Pager pager, Flag flags = Flag::Read | Flag::Write) {

    }
}