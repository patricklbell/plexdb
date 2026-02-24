module;
#include <unistd.h>
#include <string.h>

module objstore.repl;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

import objstore.engine;
import objstore.parser;

using namespace plexdb;

namespace objstore::repl {
    static constexpr const char* PROMPT = "objstore> ";

    static void fd_write(int fd, const char* data, U64 length) {
        U64 written = 0;
        while (written < length) {
            ssize_t n = write(fd, data + written, length - written);
            if (n <= 0) break;
            written += (U64)n;
        }
    }

    static void fd_write(int fd, String8 s) {
        fd_write(fd, s.data, s.length);
    }

    static void write_result(int out_fd, engine::ExecutionResult& result, U64 row_count) {
        switch (result.kind) {
            case engine::ResultKind::Void:{
                fd_write(out_fd, engine::to_str(result.status));
                if (result.message.length) {
                    fd_write(out_fd, ": ");
                    fd_write(out_fd, result.message);
                }
                fd_write(out_fd, "\n");
            }break;
            case engine::ResultKind::Rows:{
                if (row_count == 0) {
                    fd_write(out_fd, "(0 rows)\n");
                } else {
                    AutoString8 count_str = to_str(row_count);
                    fd_write(out_fd, "(");
                    fd_write(out_fd, count_str.c_str, count_str.length);
                    fd_write(out_fd, " rows)\n");
                }
            }break;
            case engine::ResultKind::SchemaChange:{
                fd_write(out_fd, engine::to_str(result.status));
                if (result.message.length) {
                    fd_write(out_fd, ": ");
                    fd_write(out_fd, result.message);
                }
                if (result.keyspace.length) {
                    fd_write(out_fd, " (keyspace: ");
                    fd_write(out_fd, result.keyspace);
                    if (result.table.length) {
                        fd_write(out_fd, ", table: ");
                        fd_write(out_fd, result.table);
                    }
                    fd_write(out_fd, ")");
                }
                fd_write(out_fd, "\n");
            }break;
        }
    }

    void run(int in_fd, int out_fd, engine::Engine& eng) {
        constexpr U64 INPUT_BUFFER_SIZE = 4096;
        char input_buf[INPUT_BUFFER_SIZE];
        
        AutoString8 pending_input;

        fd_write(out_fd, PROMPT);

        while (true) {
            ssize_t n = read(in_fd, input_buf, INPUT_BUFFER_SIZE - 1);
            if (n <= 0) break;

            input_buf[n] = '\0';

            // Accumulate input
            for (ssize_t i = 0; i < n; i++) {
                pending_input.push_back(input_buf[i]);
            }

            // Process complete statements (terminated by ';')
            while (true) {
                String8 pending{pending_input.c_str, pending_input.length};
                if (!parser::cql::is_complete(pending)) break;

                // Find the ';' – only pass up to it for parsing
                U64 semi_pos = 0;
                for (U64 i = 0; i < pending_input.length; i++) {
                    if (pending_input.c_str[i] == ';') {
                        semi_pos = i + 1;
                        break;
                    }
                }

                String8 stmt_input{pending_input.c_str, semi_pos};
                auto stmt_opt = parser::cql::parse(stmt_input, false);
                if (!stmt_opt) {
                    fd_write(out_fd, "ERROR: Failed to parse CQL\n");
                    pending_input = AutoString8{};
                    break;
                }

                U64 row_count = 0;

                const auto on_value = [&out_fd, &row_count](
                    const schema::Table& tbl,
                    const schema::Column& col,
                    U64 col_count,
                    U64 row_idx,
                    U64 col_idx,
                    const dtype::ReadValue& value
                ) {
                    // Print column header row on first row
                    if (row_idx == 0 && col_idx == 0) {
                        for (U64 c = 0; c < col_count; c++) {
                            if (c > 0) fd_write(out_fd, " | ");
                            fd_write(out_fd, tbl.cols[c].name);
                        }
                        fd_write(out_fd, "\n");
                        for (U64 c = 0; c < col_count; c++) {
                            if (c > 0) fd_write(out_fd, "-+-");
                            for (U64 j = 0; j < tbl.cols[c].name.length; j++) {
                                fd_write(out_fd, "-", 1);
                            }
                        }
                        fd_write(out_fd, "\n");
                    }

                    if (col_idx == 0) fd_write(out_fd, " ");
                    else fd_write(out_fd, " | ");

                    AutoString8 val_str = dtype::to_str(value, col.dtype);
                    fd_write(out_fd, val_str.c_str, val_str.length);

                    if (col_idx == col_count - 1) fd_write(out_fd, "\n");

                    row_count = row_idx + 1;
                };

                engine::ExecutionResult result = engine::execute(eng, *stmt_opt, on_value);
                write_result(out_fd, result, row_count);

                // Remove the processed statement and skip trailing whitespace
                while (semi_pos < pending_input.length &&
                       (pending_input.c_str[semi_pos] == ' ' ||
                        pending_input.c_str[semi_pos] == '\n' ||
                        pending_input.c_str[semi_pos] == '\r' ||
                        pending_input.c_str[semi_pos] == '\t')) {
                    semi_pos++;
                }

                AutoString8 remainder;
                for (U64 i = semi_pos; i < pending_input.length; i++) {
                    remainder.push_back(pending_input.c_str[i]);
                }
                pending_input = remainder;
            }

            fd_write(out_fd, PROMPT);
        }
    }
}
