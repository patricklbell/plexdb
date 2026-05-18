module;
#include <plexdb/macros/macros.h>
#include <coroutine>

module cql.repl;

import plexdb.base;
import plexdb.aio;
import plexdb.os;
import plexdb.tagged_union;

import cql.engine;
import cql.engine.io; // @note require for to_str on database value
import cql.parsers;

using namespace plexdb;

namespace cql::repl {
    static constexpr const char* PROMPT = "cql> ";

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
            case engine::ResultKind::UseKeyspace:
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
            case engine::ResultKind::VirtualRows:{
                AutoString8 count_str = to_str(row_count);
                os::stream_write(ostream, "(");
                os::stream_write(ostream, count_str.c_str, count_str.length);
                os::stream_write(ostream, " rows)\n");
            }break;
        }
    }

    void run(os::Handle istream, os::Handle ostream, Engine& eng) {
        aio::EventConsumer repl_consumer{0, aio::OnUnblockFunctor{[](const TArrayView<os::PollEvent>&) -> bool { return true; }}};
        os::Poll repl_poll{};
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
                if (!find(pending_input, ';')) {
                    break;
                }

                Optional<U64> semi_idx_opt = find(pending_input, ';');
                String8 stmt_input{pending_input.data, (static_cast<bool>(semi_idx_opt)) ? (*semi_idx_opt + 1) : 0};

                auto stmt_opt = parsers::parse(stmt_input);
                if (!stmt_opt) {
                    os::stream_write(ostream, "ERROR: Failed to parse CQL\n");
                    pending_input = AutoString8{};
                    break;
                }

                engine::ExecutionResult result = aio::drive(engine::execute(eng, *stmt_opt), repl_consumer, repl_poll);

                U64 row_count = 0;
                if (result.rows && result.resolved_table) {
                    const schema::Table* tbl = result.resolved_table;
                    bool has_select = result.select_col_indices.length > 0;

                    auto is_selected = [&](U64 ci) -> bool {
                        if (!has_select) return true;
                        for (U64 k = 0; k < result.select_col_indices.length; k++)
                            if (result.select_col_indices[k] == ci) return true;
                        return false;
                    };

                    // print header in schema order, filtered by selection
                    {
                        bool first = true;
                        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                            if (!is_selected(ci)) continue;
                            if (!first) os::stream_write(ostream, " | ");
                            os::stream_write(ostream, tbl->cols[ci].name);
                            first = false;
                        }
                        os::stream_write(ostream, "\n");
                        first = true;
                        for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                            if (!is_selected(ci)) continue;
                            if (!first) os::stream_write(ostream, "-+-");
                            for (U64 j = 0; j < tbl->cols[ci].name.length; j++)
                                os::stream_write(ostream, "-", 1);
                            first = false;
                        }
                        os::stream_write(ostream, "\n");
                    }

                    U64 row_limit = result.row_limit_count;
                    RowIterator& row_it  = result.rows->start;
                    RowIterator& row_end = result.rows->stop;
                    while (row_it != row_end && row_count < row_limit) {
                        ColumnRange col_range = aio::drive(row_it.deref(), repl_consumer, repl_poll);
                        bool first = true;
                        for (U64 ci = 0; ci < tbl->cols.length && col_range.start != col_range.stop; ci++, ++col_range.start) {
                            if (!is_selected(ci)) continue;
                            os::stream_write(ostream, first ? " " : " | ");
                            AutoString8 val_str = to_str(*col_range.start, tbl->cols[ci].type);
                            os::stream_write(ostream, val_str.c_str, val_str.length);
                            first = false;
                        }
                        os::stream_write(ostream, "\n");
                        aio::drive(row_it.advance(), repl_consumer, repl_poll);
                        ++row_count;
                    }
                } else if (result.virtual_rows) {
                    auto& vr = *result.virtual_rows;
                    if (vr.columns.length > 0) {
                        for (U64 ci = 0; ci < vr.columns.length; ci++) {
                            if (ci > 0) os::stream_write(ostream, " | ");
                            os::stream_write(ostream, vr.columns[ci].name);
                        }
                        os::stream_write(ostream, "\n");
                        for (U64 ci = 0; ci < vr.columns.length; ci++) {
                            if (ci > 0) os::stream_write(ostream, "-+-");
                            for (U64 j = 0; j < vr.columns[ci].name.length; j++)
                                os::stream_write(ostream, "-", 1);
                        }
                        os::stream_write(ostream, "\n");
                    }
                    for (U64 ri = 0; ri < vr.rows.length; ri++) {
                        for (U64 ci = 0; ci < vr.columns.length; ci++) {
                            os::stream_write(ostream, ci == 0 ? " " : " | ");
                            AutoString8 val_str = to_str(vr.rows[ri].values[ci], vr.columns[ci].type);
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

    void run(Engine& engine) {
        run(os::stdin_stream(), os::stdout_stream(), engine);
    }
}
