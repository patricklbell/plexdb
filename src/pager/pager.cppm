module;

export module plexdb.pager;

import plexdb.base;
import plexdb.os;

export namespace plexdb::pager {
    enum Lock {
        Unlocked,
        Shared,
        Reserved,
        // Pending, (always transitions to exclusive or fails)
        Exclusive,
    };

    enum Mode {
        JRNL,
    };

    enum JournalMode {
        Delete,
        Truncate,
        Persist,
    };

    // contains the settings for the pager and persistent data across transactions
    struct Pager {
        Mode mode;
        JournalMode journal_mode;
    };
    export Pager create_journal_pager();

    // represents an atomic write to a file, consisting of a set of differently 
    // sized pages
    struct Transaction {
        os::Handle commit_file;
    };
    export Transaction begin_transaction(const Pager& pager, os::Handle file);
    export void add_page(Transaction& transaction, U64 page_type, U64 page_number, U8* page);
    export void end_transaction(Transaction& transaction);
}