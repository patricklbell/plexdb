export module cql.engine.column_value;

// ColumnValue/NestedColumnValue and the type::Basic-keyed compare/to_str overloads now live in
// cql.column_types (engine-independent — see that module for why). This module re-exports them
// unchanged for existing `import cql.engine.column_value;` call sites, and adds only the
// type::Type-aware to_str overload below, which genuinely needs the engine's schema type tree
// (Tuple/UDT/Vector/List element recursion) that cql.column_types deliberately doesn't depend on.
export import cql.column_types;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.types;

using namespace plexdb;
using namespace cql::column_types;

export namespace plexdb {
    AutoString8 to_str(const cql::column_types::ColumnValue& value, const cql::type::Type& cdtype);
}
