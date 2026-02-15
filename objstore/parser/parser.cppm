export module objstore.parser;

import plexdb.base;
import plexdb.variant;
import objstore.dtypes;
import objstore.parser.stl_helpers;

using namespace plexdb;

export namespace objstore::parser {
    // ========================================================================
    // http
    // ========================================================================
    constexpr U64 MAX_HEADERS = 32;

    struct Header {
        String8 name;
        String8 value;
    };

    struct HttpRequest {
        String8 method;
        String8 url;
        Array<Header, MAX_HEADERS> headers;
        U64 header_count = 0;
        String8 body;
    };

    // Parse a raw buffer into an HTTP request.
    // Returns Ok only if fully valid and complete.
    Optional<HttpRequest> parse_request(String8 bytes);

    // ========================================================================
    // cassandra query language (CQL)
    // ========================================================================
    enum class CqlQueryType : U8 {
        CreateKeyspace,
        CreateTable,
        InsertInto,
        SelectFrom,
    };

    constexpr U64 MAX_KEYSPACE_OPTIONS = 32;

    struct CreateKeyspaceRequestOption {
        String8 key;
        TaggedUnion<STLString, S64> value;
    };

    struct CreateKeyspaceRequest {
        String8 keyspace_name;
        bool if_not_exists;
        STLArray<CreateKeyspaceRequestOption, MAX_KEYSPACE_OPTIONS> options;
    };

    struct CreateTableRequestColumn {
        String8 name;
        DType dtype;
        bool is_primary_key;
    };

    struct CreateTableRequest {
        String8 table_name;
        bool if_not_exists;
        STLDynamicArray<CreateKeyspaceRequestOption> columns;
    };

    // @todo support auto increment etc.
    struct InsertIntoRequestValue {
        DType dtype;
        TaggedUnion<STLString, S64> value;
    };

    struct InsertIntoRequest {
        String8 keyspace_name;
        String8 table_name;
        STLDynamicArray<InsertIntoRequestValue> values;
    };

    struct SelectFromRequest {
        String8 keyspace_name;
        String8 table_name;
        // @todo
    };

    struct CqlRequest {
        CqlQueryType query_type;

        union {
            CreateKeyspaceRequest create_keyspace;
            CreateTableRequest create_table;
            InsertIntoRequest insert_into;
            SelectFromRequest select_from;
        };

        ~CqlRequest() {
            switch(query_type) {
                case CqlQueryType::CreateKeyspace:{
                    create_keyspace.~CreateKeyspaceRequest();
                }break;
                case CqlQueryType::CreateTable:{
                    create_table.~CreateTableRequest();
                }break;
                case CqlQueryType::InsertInto:{
                    insert_into.~InsertIntoRequest();
                }break;
                case CqlQueryType::SelectFrom:{
                    select_from.~SelectFromRequest();
                }break;
            }
        }
    };

    // Parses a CQL request
    Optional<CqlRequest> parse_cql(String8 bytes);
}
