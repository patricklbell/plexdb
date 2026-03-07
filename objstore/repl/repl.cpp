module;
#include "macros.h"

module objstore.repl;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

import objstore.engine;
import objstore.parser;

using namespace plexdb;

namespace objstore::repl {
    static constexpr const char* PROMPT = "objstore> ";

    static void write_result(os::Handle ostream, engine::ExecutionResult& result, U64 row_count) {
        switch (result.kind) {
            case engine::ResultKind::Void:{
                os::stream_write(ostream, engine::to_str(result.status));
                if (result.message.length) {
                    os::stream_write(ostream, ": ");
                    os::stream_write(ostream, result.message);
                }
                os::stream_write(ostream, "\n");
            }break;
            case engine::ResultKind::Rows:{
                if (row_count == 0) {
                    os::stream_write(ostream, "(0 rows)\n");
                } else {
                    AutoString8 count_str = to_str(row_count);
                    os::stream_write(ostream, "(");
                    os::stream_write(ostream, count_str.c_str, count_str.length);
                    os::stream_write(ostream, " rows)\n");
                }
            }break;
            case engine::ResultKind::SchemaChange:{
                os::stream_write(ostream, engine::to_str(result.status));
                if (result.message.length) {
                    os::stream_write(ostream, ": ");
                    os::stream_write(ostream, result.message);
                }
                if (result.keyspace.length) {
                    os::stream_write(ostream, " (keyspace: ");
                    os::stream_write(ostream, result.keyspace);
                    if (result.table.length) {
                        os::stream_write(ostream, ", table: ");
                        os::stream_write(ostream, result.table);
                    }
                    os::stream_write(ostream, ")");
                }
                os::stream_write(ostream, "\n");
            }break;
        }
    }

    void run(os::Handle istream, os::Handle ostream, engine::Engine& eng) {
        constexpr U64 INPUT_BUFFER_SIZE = 4096;
        char input_buf[INPUT_BUFFER_SIZE];
        
        os::stream_write(ostream, PROMPT);

        while (true) {
            U64 n = os::stream_read(istream, input_buf, INPUT_BUFFER_SIZE - 1);
            if (n == 0) {
                break;
            }

            input_buf[n] = '\0';
            String8 pending_input(input_buf, n);

            while (true) {
                if (!parser::cql::is_complete(pending_input)) {
                    break;
                }

                Optional<U64> semi_idx_opt = find(pending_input, ';');
                String8 stmt_input{pending_input.data, (static_cast<bool>(semi_idx_opt)) ? (*semi_idx_opt + 1) : 0};

                auto stmt_opt = parser::cql::parse(stmt_input, false);
                if (!stmt_opt) {
                    os::stream_write(ostream, "ERROR: Failed to parse CQL\n");
                    pending_input = AutoString8{};
                    break;
                }

                engine::ExecutionResult result = engine::execute(eng, *stmt_opt);
                
                U64 row_count = 0;
                if (result.rows) {
                    for (auto& row = result.rows->begin(); row != result.rows->end(); ++row) {
                        if (row_count == 0) {
                            U64 col_idx = 0;
                            for (auto col = engine::columns_begin(row); col != engine::columns_end(row); ++col, ++col_idx) {
                                if (col_idx > 0) os::stream_write(ostream, " | ");
                                os::stream_write(ostream, engine::column(col).name);
                            }
                            os::stream_write(ostream, "\n");

                            col_idx = 0;
                            for (auto col = engine::columns_begin(row); col != engine::columns_end(row); ++col, ++col_idx) {
                                if (col_idx > 0) os::stream_write(ostream, "-+-");
                                for (U64 j = 0; j < engine::column(col).name.length; j++)
                                    os::stream_write(ostream, "-", 1);
                            }
                            os::stream_write(ostream, "\n");
                        }

                        U64 col_idx = 0;
                        for (auto col = engine::columns_begin(row); col != engine::columns_end(row); ++col, ++col_idx) {
                            os::stream_write(ostream, col_idx == 0 ? " " : " | ");
                            AutoString8 val_str = dtype::to_str(engine::read_value(col), engine::column(col).dtype);
                            os::stream_write(ostream, val_str.c_str, val_str.length);
                        }
                        os::stream_write(ostream, "\n");
                        ++row_count;
                    }
                }
                
                write_result(ostream, result, row_count);

                // @note guaranteed to be valid index since there is a trailing \0
                pending_input = String8(&pending_input.data[stmt_input.length], pending_input.length - stmt_input.length);
            }

            os::stream_write(ostream, PROMPT);
        }
    }

    void run(engine::Engine& engine) {
        run(os::stdin_stream(), os::stdout_stream(), engine);
    }
}
