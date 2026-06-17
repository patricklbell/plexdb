export module cql.engine.system_schema;

import plexdb.base;
import plexdb.os;

import cql.engine.virtual_table;
import cql.engine.schema;

using namespace plexdb;

export namespace cql::engine {
    // @todo Cassandra stores system_schema as real (not virtual) tables, this
    // is probably a good idea. until then, fake it with virtual tables
    VirtualRows create_schema_keyspaces(schema::Schema& schema);
    VirtualRows create_schema_tables(schema::Schema& schema);
    VirtualRows create_schema_columns(schema::Schema& schema);
    VirtualRows create_schema_views(schema::Schema& schema);
    VirtualRows create_schema_indexes(schema::Schema& schema);
    VirtualRows create_schema_triggers(schema::Schema& schema);
    VirtualRows create_schema_dropped_columns(schema::Schema& schema);
    VirtualRows create_schema_types(schema::Schema& schema);
    VirtualRows create_schema_functions(schema::Schema& schema);
    VirtualRows create_schema_aggregates(schema::Schema& schema);

    VirtualRows create_system_local(U16 port, schema::Schema& schema);
    VirtualRows create_system_peers();
    VirtualRows create_system_peers_v2();
}
