export module keyvalue.engine.statements;

import plexdb.base;
import plexdb.dynamic.containers;
import plexdb.os;
import plexdb.tagged_union;

using namespace plexdb;

export namespace keyvalue {
    struct Quit {};
    struct FlushDb {};
    struct FlushAll {};
    struct DbSize {};

    struct Ping {
        Optional<AutoString8> message;
    };
    struct Get {
        AutoString8 key;
    };
    struct Set {
        AutoString8 key;
        AutoString8 value;
        bool        nx;
        bool        xx;
    };
    struct Del {
        DynamicArray<AutoString8> keys;
    };
    struct Exists {
        DynamicArray<AutoString8> keys;
    };
    struct Mget {
        DynamicArray<AutoString8> keys;
    };
    struct Mset {
        DynamicArray<Pair<AutoString8, AutoString8>> pairs;
    };
    struct Keys {
        AutoString8 pattern;
    };
    struct Scan {
        U64                   cursor;
        Optional<AutoString8> match;
        Optional<U64>         count;
    };
    struct TypeOf {
        AutoString8 key;
    };
    struct Cmd {};
    struct ClientGetName {};
    struct SelectDb {
        U64 index;
    };
    struct Client {
        DynamicArray<AutoString8> args;
    };
    struct Info {
        Optional<AutoString8> section;
    };
    struct Unknown {
        AutoString8               name;
        DynamicArray<AutoString8> args;
    };

    struct Statement {
        TaggedUnion<
            Ping, Quit, Get, Set,
            Del, Exists, Mget, Mset,
            Keys, Scan,
            FlushDb, FlushAll, DbSize,
            TypeOf, Cmd, SelectDb, ClientGetName, Client, Info,
            Unknown>
            value;
    };
}
