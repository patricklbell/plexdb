module;
#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/dsl/expression.hpp>
#include <lexy/error.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/input_location.hpp>

export module plexdb.support.lexy.error_callback;

import plexdb.base;

namespace plexdb::support::lexy {
    template<typename Input, typename Reader, typename Tag>
    inline AutoString8 format_lexy_error(const ::lexy::error_context<Input>& context, const ::lexy::error<Reader, Tag>& error) {
        auto location = ::lexy::get_input_location(context.input(), error.position());

        if constexpr (SameAs<Tag, ::lexy::expected_literal>) {
            return fmt("error: while parsing %s at %u:%u: expected '%.*s'", context.production(), location.line_nr(), location.column_nr(), static_cast<int>(error.length()), reinterpret_cast<const char*>(error.string()));
        } else if constexpr (SameAs<Tag, ::lexy::expected_keyword>) {
            return fmt("error: while parsing %s at %u:%u: expected keyword '%.*s'", context.production(), location.line_nr(), location.column_nr(), static_cast<int>(error.length()), reinterpret_cast<const char*>(error.string()));
        } else if constexpr (SameAs<Tag, ::lexy::expected_char_class>) {
            return fmt("error: while parsing %s at %u:%u: expected %s", context.production(), location.line_nr(), location.column_nr(), error.name());
        } else {
            return fmt("error: while parsing %s at %u:%u: %s", context.production(), location.line_nr(), location.column_nr(), error.message());
        }
    }

    export template<typename ErrorFn>
    struct ErrorCallback {
        struct _sink {
            using return_type = size_t;
            size_t  _count    = 0;
            ErrorFn fn;

            template<typename Input, typename Reader, typename Tag>
            void operator()(const ::lexy::error_context<Input>& context, const ::lexy::error<Reader, Tag>& error) {
                fn(format_lexy_error(context, error));
                ++_count;
            }

            size_t finish() && {
                return _count;
            }
        };

        ErrorFn fn;

        auto sink() const {
            return _sink{0, fn};
        }
    };

    /// @note Captures only the first emitted error into @p out; subsequent errors (from
    /// implicit recovery in dsl::list / dsl::delimited) are dropped. lexy already cancels
    /// on the first fatal error by default; this sink ensures the boundary surface is
    /// a single message even when recovery emits a cascade.
    export struct FirstErrorSink {
        struct _sink {
            using return_type = size_t;
            AutoString8* out;
            size_t       count = 0;

            template<typename Input, typename Reader, typename Tag>
            void operator()(const ::lexy::error_context<Input>& context, const ::lexy::error<Reader, Tag>& error) {
                if (count == 0) {
                    *out = format_lexy_error(context, error);
                }
                ++count;
            }

            size_t finish() && {
                return count;
            }
        };

        AutoString8* out;

        auto sink() const {
            return _sink{out, 0};
        }
    };
}
