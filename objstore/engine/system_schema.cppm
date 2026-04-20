export module objstore.engine.system_schema;

import plexdb.base;
import plexdb.os;

import objstore.engine.virtual_table;
import objstore.engine.schema;

using namespace plexdb;

export namespace objstore::engine {
    // @todo Cassandra stores system_schema as real (not virtual) tables, this 
    // is probably a good idea. until then, fake it with virtual tables
    VirtualRows make_schema_keyspaces(schema::Schema& schema);
    VirtualRows make_schema_tables(schema::Schema& schema);
    VirtualRows make_schema_columns(schema::Schema& schema);
    VirtualRows make_schema_views(schema::Schema& schema);
    VirtualRows make_schema_indexes(schema::Schema& schema);
    VirtualRows make_schema_triggers(schema::Schema& schema);
    VirtualRows make_schema_dropped_columns(schema::Schema& schema);
    VirtualRows make_schema_types(schema::Schema& schema);
    VirtualRows make_schema_functions(schema::Schema& schema);
    VirtualRows make_schema_aggregates(schema::Schema& schema);

    VirtualRows make_system_local();
    VirtualRows make_system_peers();
    VirtualRows make_system_peers_v2();
}
