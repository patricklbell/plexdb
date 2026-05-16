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
    export template<typename ErrorFn>
    struct ErrorCallback {
        struct _sink {
            using return_type = size_t;
            size_t _count = 0;
            ErrorFn fn;

            template <typename Input, typename Reader, typename Tag>
            void operator()(const ::lexy::error_context<Input>& context, const ::lexy::error<Reader, Tag>& error) {
                auto location = ::lexy::get_input_location(context.input(), error.position());

                AutoString8 msg;
                if constexpr (SameAs<Tag, ::lexy::expected_literal>) {
                    msg = fmt("error: while parsing %s at %u:%u: expected '%.*s'",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              static_cast<int>(error.length()), reinterpret_cast<const char*>(error.string()));
                } else if constexpr (SameAs<Tag, ::lexy::expected_keyword>) {
                    msg = fmt("error: while parsing %s at %u:%u: expected keyword '%.*s'",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              static_cast<int>(error.length()), reinterpret_cast<const char*>(error.string()));
                } else if constexpr (SameAs<Tag, ::lexy::expected_char_class>) {
                    msg = fmt("error: while parsing %s at %u:%u: expected %s",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              error.name());
                } else {
                    msg = fmt("error: while parsing %s at %u:%u: %s",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              error.message());
                }

                fn(msg);
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
}
