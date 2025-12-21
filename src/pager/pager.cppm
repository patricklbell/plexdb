module;

export module plexdb.pager;

import plexdb.base;
import plexdb.os;

export namespace plexdb::pager {
    // contains the settings for the pager and persistent data across transactions
    struct Pager {
        os::Handle file;

        Pager(os::Handle file) : file(file) {}
        ~Pager() { os::file_close(file); }
    };
    export Pager create(String8 file_path);

    enum Flag {
        Read  = 1 << 0;
        Write = 1 << 1;
    }
    export Page open(Pager pager, Flag flags = Flag::Read | Flag::Write);
}