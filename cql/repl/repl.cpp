module;
#include <plexdb/macros/macros.h>
#include <coroutine>

module cql.repl;

import plexdb.base;
import plexdb.aio;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.coroutine;
import cql.engine;
import cql.engine.column_value;
import cql.engine.io; // @note require for to_str on database value
import cql.engine.statements;
import cql.engine.evaluator;
import cql.parsers;

using namespace plexdb;

namespace cql::repl {
    static constexpr const char* PROMPT = "cql> ";

    static void write_result(os::Handle ostream, engine::ExecutionResult& result, U64 row_count) {
        switch (result.kind) {
            case engine::ResultKind::Void: {
                os::stream_write(ostream, engine::to_str(result.status));
                if (result.message.length) {
                    os::stream_write(ostream, ": ");
                    os::stream_write(ostream, result.message);
                }
                os::stream_write(ostream, "\n");
            } break;
            case engine::ResultKind::Rows: {
                if (row_count == 0) {
                    os::stream_write(ostream, "(0 rows)\n");
                } else {
                    AutoString8 count_str = to_str(row_count);
                    os::stream_write(ostream, "(");
                    os::stream_write(ostream, count_str.c_str, count_str.length);
                    os::stream_write(ostream, " rows)\n");
                }
            } break;
            case engine::ResultKind::UseKeyspace:
            case engine::ResultKind::SchemaChange: {
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
            } break;
            case engine::ResultKind::VirtualRows: {
                AutoString8 count_str = to_str(row_count);
                os::stream_write(ostream, "(");
                os::stream_write(ostream, count_str.c_str, count_str.length);
                os::stream_write(ostream, " rows)\n");
            } break;
        }
    }

    void run(os::Handle istream, os::Handle ostream, Engine& engine) {
        aio::EventConsumer repl_consumer{0, aio::OnUnblockFunctor{[](const TArrayView<os::PollEvent>&) -> bool { return true; }}};
        os::Poll           repl_poll{};
        constexpr U64      INPUT_BUFFER_SIZE = 4096;
        char               input_buf[INPUT_BUFFER_SIZE];

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
                String8       stmt_input{pending_input.data, (static_cast<bool>(semi_idx_opt)) ? (*semi_idx_opt + 1) : 0};

                auto stmt_opt = parsers::parse(stmt_input);
                if (!stmt_opt) {
                    os::stream_write(ostream, "ERROR: Failed to parse CQL\n");
                    pending_input = AutoString8{};
                    break;
                }

                const auto execute_cql_and_write = [&]() -> coroutine::Task<> {
                    engine::ExecutionResult result = co_await engine::execute(engine, *stmt_opt);

                    U64 row_count = 0;
                    if (result.rows && result.resolved_table) {
                        const schema::Table* tbl        = result.resolved_table;
                        bool                 has_select = result.select_col_indices.length > 0;

                        auto is_selected = [&](U64 ci) -> bool {
                            if (!has_select) {
                                return true;
                            }
                            for (U64 k = 0; k < result.select_col_indices.length; k++) {
                                if (result.select_col_indices[k] == ci) {
                                    return true;
                                }
                            }
                            return false;
                        };

                        // print header in schema order, filtered by selection
                        {
                            bool first = true;
                            for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                if (!is_selected(ci)) {
                                    continue;
                                }
                                if (!first) {
                                    os::stream_write(ostream, " | ");
                                }
                                os::stream_write(ostream, tbl->cols[ci].name);
                                first = false;
                            }
                            os::stream_write(ostream, "\n");
                            first = true;
                            for (U64 ci = 0; ci < tbl->cols.length; ci++) {
                                if (!is_selected(ci)) {
                                    continue;
                                }
                                if (!first) {
                                    os::stream_write(ostream, "-+-");
                                }
                                for (U64 j = 0; j < tbl->cols[ci].name.length; j++) {
                                    os::stream_write(ostream, "-", 1);
                                }
                                first = false;
                            }
                            os::stream_write(ostream, "\n");
                        }

                        U64          row_limit  = result.row_limit_count;
                        RowIterator& row_it     = result.rows->start;
                        RowIterator& row_end    = result.rows->stop;
                        bool         has_filter = result.filter_predicates.length > 0;
                        while (row_it != row_end && row_count < row_limit) {
                            ColumnRange col_range = co_await row_it.deref();

                            if (has_filter) {
                                DynamicArray<ColumnValue> row_values;
                                while (col_range.start != col_range.stop && row_values.length < tbl->cols.length) {
                                    push_back(row_values, co_await col_range.start.deref());
                                    co_await col_range.start.advance();
                                }
                                EvalContext row_ctx = result.filter_ctx;
                                row_ctx.table       = result.resolved_table;
                                row_ctx.row_values  = row_values.ptr;
                                if (!evaluate_where(result.filter_predicates, row_ctx)) {
                                    co_await row_it.advance(row_end);
                                    continue;
                                }
                                bool first = true;
                                for (U64 ci2 = 0; ci2 < tbl->cols.length; ci2++) {
                                    if (!is_selected(ci2)) {
                                        continue;
                                    }
                                    AutoString8 val_str = to_str(row_values.ptr[ci2], tbl->cols[ci2].type);
                                    os::stream_write(ostream, first ? " " : " | ");
                                    os::stream_write(ostream, val_str.c_str, val_str.length);
                                    first = false;
                                }
                            } else {
                                bool first = true;
                                U64  ci    = 0;
                                while (col_range.start != col_range.stop && ci < tbl->cols.length) {
                                    if (is_selected(ci)) {
                                        AutoString8 val_str = to_str(co_await col_range.start.deref(), tbl->cols[ci].type);
                                        os::stream_write(ostream, first ? " " : " | ");
                                        os::stream_write(ostream, val_str.c_str, val_str.length);
                                        first = false;
                                    }
                                    co_await col_range.start.advance();
                                    ++ci;
                                }
                            }
                            os::stream_write(ostream, "\n");
                            co_await row_it.advance(row_end);
                            ++row_count;
                        }
                    } else if (result.virtual_rows) {
                        auto& vr = *result.virtual_rows;
                        if (vr.columns.length > 0) {
                            for (U64 ci = 0; ci < vr.columns.length; ci++) {
                                if (ci > 0) {
                                    os::stream_write(ostream, " | ");
                                }
                                os::stream_write(ostream, vr.columns[ci].name);
                            }
                            os::stream_write(ostream, "\n");
                            for (U64 ci = 0; ci < vr.columns.length; ci++) {
                                if (ci > 0) {
                                    os::stream_write(ostream, "-+-");
                                }
                                for (U64 j = 0; j < vr.columns[ci].name.length; j++) {
                                    os::stream_write(ostream, "-", 1);
                                }
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

                    if (result.deferred_tx.started_transaction) {
                        co_await result.deferred_tx.commit();
                    }

                    write_result(ostream, result, row_count);
                };

                aio::drive(execute_cql_and_write(), repl_consumer, repl_poll);

                // @note guaranteed to be valid index since there is a trailing \0
                pending_input = String8(&pending_input.data[stmt_input.length], pending_input.length - stmt_input.length);
                os::stream_write(ostream, PROMPT);
            }
        }
    }

    void run(Engine& engine) {
        run(os::stdin_stream(), os::stdout_stream(), engine);
    }
}
