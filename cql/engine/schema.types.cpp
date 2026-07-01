module;
#include <plexdb/macros/macros.h>
#include <coroutine>

module cql.engine.schema.types;

import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

using namespace plexdb;

namespace cql::schema {
    U64 serialized_type_size(const type::Type& t) {
        return visit(t.value, [](const auto& v) -> U64 {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                return 1;
            } else if constexpr (SameAs<T, type::List>) {
                return 1 + 1 + serialized_type_size(v.element);
            } else if constexpr (SameAs<T, type::Set>) {
                return 1 + 1 + serialized_type_size(v.key);
            } else if constexpr (SameAs<T, type::Map>) {
                return 1 + 1 + serialized_type_size(v.key) + serialized_type_size(v.value);
            } else if constexpr (SameAs<T, type::Vector>) {
                return 1 + 1 + sizeof(U64) + serialized_type_size(v.element);
            } else if constexpr (SameAs<T, type::Tuple>) {
                U64 total = 1 + 1 + sizeof(U64);
                for (const auto& e : v.elements) {
                    total += serialized_type_size(e);
                }
                return total;
            } else {
                static_assert(SameAs<T, type::UDT*>, "unhandled Type variant in serialized_type_size");
                assert_true(v != nullptr, "serialize UDT pointer is null");
                return 1 + 1 + sizeof(U64) + v->name.length;
            }
        });
    }

    static coroutine::Task<void> put_u8(blob::BlobDynamicPaged& blob, U8 v, U64* offset) {
        co_await blob::update(blob, &v, 1, *offset);
        *offset += 1;
    }
    static coroutine::Task<void> put_u64(blob::BlobDynamicPaged& blob, U64 v, U64* offset) {
        co_await blob::update(blob, reinterpret_cast<const U8*>(&v), sizeof(v), *offset);
        *offset += sizeof(v);
    }
    static coroutine::Task<void> put_bytes(blob::BlobDynamicPaged& blob, const U8* data, U64 length, U64* offset) {
        co_await blob::update(blob, data, length, *offset);
        *offset += length;
    }
    static coroutine::Task<U8> get_u8(blob::BlobDynamicPaged& blob, U64* offset) {
        U8 v = 0;
        co_await blob::get(blob, &v, 1, *offset);
        *offset += 1;
        co_return v;
    }
    static coroutine::Task<U64> get_u64(blob::BlobDynamicPaged& blob, U64* offset) {
        U64 v = 0;
        co_await blob::get(blob, reinterpret_cast<U8*>(&v), sizeof(v), *offset);
        *offset += sizeof(v);
        co_return v;
    }

    coroutine::Task<void> write_type(blob::BlobDynamicPaged& blob, const type::Type& t, U64* offset) {
        if (type_matches_tag<type::Basic>(t.value)) {
            co_await put_u8(blob, static_cast<U8>(get<type::Basic>(t.value)), offset);
            co_return;
        }
        if (type_matches_tag<type::List>(t.value)) {
            const auto& v = get<type::List>(t.value);
            co_await put_u8(blob, static_cast<U8>(TypeKind::List), offset);
            co_await put_u8(blob, v.frozen ? 1 : 0, offset);
            co_await write_type(blob, v.element, offset);
            co_return;
        }
        if (type_matches_tag<type::Set>(t.value)) {
            const auto& v = get<type::Set>(t.value);
            co_await put_u8(blob, static_cast<U8>(TypeKind::Set), offset);
            co_await put_u8(blob, v.frozen ? 1 : 0, offset);
            co_await write_type(blob, v.key, offset);
            co_return;
        }
        if (type_matches_tag<type::Map>(t.value)) {
            const auto& v = get<type::Map>(t.value);
            co_await put_u8(blob, static_cast<U8>(TypeKind::Map), offset);
            co_await put_u8(blob, v.frozen ? 1 : 0, offset);
            co_await write_type(blob, v.key, offset);
            co_await write_type(blob, v.value, offset);
            co_return;
        }
        if (type_matches_tag<type::Vector>(t.value)) {
            const auto& v = get<type::Vector>(t.value);
            co_await put_u8(blob, static_cast<U8>(TypeKind::Vector), offset);
            co_await put_u8(blob, v.frozen ? 1 : 0, offset);
            co_await put_u64(blob, v.count, offset);
            co_await write_type(blob, v.element, offset);
            co_return;
        }
        if (type_matches_tag<type::Tuple>(t.value)) {
            const auto& v = get<type::Tuple>(t.value);
            co_await put_u8(blob, static_cast<U8>(TypeKind::Tuple), offset);
            co_await put_u8(blob, v.frozen ? 1 : 0, offset);
            co_await put_u64(blob, v.elements.length, offset);
            for (const auto& e : v.elements) {
                co_await write_type(blob, e, offset);
            }
            co_return;
        }
        assert_true(type_matches_tag<type::UDT*>(t.value), "unhandled Type variant in write_type");
        type::UDT* u = get<type::UDT*>(t.value);
        assert_true(u != nullptr, "serialize UDT pointer is null");
        co_await put_u8(blob, static_cast<U8>(TypeKind::Udt), offset);
        co_await put_u8(blob, 0, offset);
        co_await put_u64(blob, u->name.length, offset);
        co_await put_bytes(blob, reinterpret_cast<const U8*>(u->name.data), u->name.length, offset);
    }

    coroutine::Task<Result<type::Type>> read_type(blob::BlobDynamicPaged& blob, Keyspace* resolve_ks, U64* offset) {
        U8 raw_kind = co_await get_u8(blob, offset);
        if (raw_kind < static_cast<U8>(type::Basic::COUNT)) {
            co_return Result<type::Type>{type::create_basic(static_cast<type::Basic>(raw_kind))};
        }
        switch (static_cast<TypeKind>(raw_kind)) {
            case TypeKind::List: {
                bool fr  = (co_await get_u8(blob, offset)) != 0;
                auto sub = co_await read_type(blob, resolve_ks, offset);
                if (sub.error != Error::None) {
                    co_return Result<type::Type>{{}, sub.error, sub.message};
                }
                co_return Result<type::Type>{type::create_list(move(sub.value), fr)};
            }
            case TypeKind::Set: {
                bool fr  = (co_await get_u8(blob, offset)) != 0;
                auto sub = co_await read_type(blob, resolve_ks, offset);
                if (sub.error != Error::None) {
                    co_return Result<type::Type>{{}, sub.error, sub.message};
                }
                co_return Result<type::Type>{type::create_set(move(sub.value), fr)};
            }
            case TypeKind::Map: {
                bool fr = (co_await get_u8(blob, offset)) != 0;
                auto ke = co_await read_type(blob, resolve_ks, offset);
                if (ke.error != Error::None) {
                    co_return Result<type::Type>{{}, ke.error, ke.message};
                }
                auto va = co_await read_type(blob, resolve_ks, offset);
                if (va.error != Error::None) {
                    co_return Result<type::Type>{{}, va.error, va.message};
                }
                co_return Result<type::Type>{type::create_map(move(ke.value), move(va.value), fr)};
            }
            case TypeKind::Vector: {
                bool fr    = (co_await get_u8(blob, offset)) != 0;
                U64  count = co_await get_u64(blob, offset);
                auto sub   = co_await read_type(blob, resolve_ks, offset);
                if (sub.error != Error::None) {
                    co_return Result<type::Type>{{}, sub.error, sub.message};
                }
                co_return Result<type::Type>{type::create_vector(move(sub.value), count, fr)};
            }
            case TypeKind::Tuple: {
                bool                     fr    = (co_await get_u8(blob, offset)) != 0;
                U64                      count = co_await get_u64(blob, offset);
                DynamicArray<type::Type> elems;
                reserve(elems, count);
                for (U64 i = 0; i < count; i++) {
                    auto e = co_await read_type(blob, resolve_ks, offset);
                    if (e.error != Error::None) {
                        co_return Result<type::Type>{{}, e.error, e.message};
                    }
                    push_back(elems, move(e.value));
                }
                co_return Result<type::Type>{type::create_tuple(move(elems), fr)};
            }
            case TypeKind::Udt: {
                [[maybe_unused]] U8 fr          = co_await get_u8(blob, offset);
                U64                 name_length = co_await get_u64(blob, offset);
                AutoString8         name(name_length);
                co_await blob::get(blob, reinterpret_cast<U8*>(name.c_str), name_length, *offset);
                *offset += name_length;
                if (resolve_ks == nullptr) {
                    co_return Result<type::Type>{{}, Error::MissingKeyspace, "UDT reference encountered with no keyspace context"};
                }
                String8           nm{name.c_str, name.length};
                type::UDT* const* p = find(resolve_ks->udts_by_name, nm);
                if (p == nullptr) {
                    thread_local AutoString8 msg;
                    msg = AutoString8("UDT '") + AutoString8(nm) + "' not found in keyspace";
                    co_return Result<type::Type>{{}, Error::MissingType, String8(msg)};
                }
                co_return Result<type::Type>{type::create_udt(*p)};
            }
        }
        assert_true(false, "invalid TypeKind in read_type (blob corruption)");
        co_return Result<type::Type>{type::create_basic(type::Basic::blob), Error::InvalidOptions, "corrupt type kind"};
    }

    Result<type::Type> resolve_type_ast(Schema& schema, String8 default_ks, const type::ast::Type& ast) {
        return visit(ast.value, [&](const auto& v) -> Result<type::Type> {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                return Result<type::Type>{type::create_basic(v)};
            } else if constexpr (SameAs<T, type::ast::ListAst>) {
                auto sub = resolve_type_ast(schema, default_ks, v.element);
                if (sub.error != Error::None) {
                    return Result<type::Type>{{}, sub.error, sub.message};
                }
                return Result<type::Type>{type::create_list(move(sub.value), v.frozen)};
            } else if constexpr (SameAs<T, type::ast::SetAst>) {
                auto sub = resolve_type_ast(schema, default_ks, v.key);
                if (sub.error != Error::None) {
                    return Result<type::Type>{{}, sub.error, sub.message};
                }
                return Result<type::Type>{type::create_set(move(sub.value), v.frozen)};
            } else if constexpr (SameAs<T, type::ast::MapAst>) {
                auto ke = resolve_type_ast(schema, default_ks, v.key);
                if (ke.error != Error::None) {
                    return Result<type::Type>{{}, ke.error, ke.message};
                }
                auto va = resolve_type_ast(schema, default_ks, v.value);
                if (va.error != Error::None) {
                    return Result<type::Type>{{}, va.error, va.message};
                }
                return Result<type::Type>{type::create_map(move(ke.value), move(va.value), v.frozen)};
            } else if constexpr (SameAs<T, type::ast::VectorAst>) {
                auto sub = resolve_type_ast(schema, default_ks, v.element);
                if (sub.error != Error::None) {
                    return Result<type::Type>{{}, sub.error, sub.message};
                }
                return Result<type::Type>{type::create_vector(move(sub.value), v.count, v.frozen)};
            } else if constexpr (SameAs<T, type::ast::TupleAst>) {
                DynamicArray<type::Type> elems;
                reserve(elems, v.elements.length);
                for (const auto& e : v.elements) {
                    auto sub = resolve_type_ast(schema, default_ks, e);
                    if (sub.error != Error::None) {
                        return Result<type::Type>{{}, sub.error, sub.message};
                    }
                    push_back(elems, move(sub.value));
                }
                return Result<type::Type>{type::create_tuple(move(elems), v.frozen)};
            } else {
                static_assert(SameAs<T, type::ast::UdtRef>, "unhandled ast::Type variant in resolve_type_ast");
                String8 ks_name = v.keyspace.length > 0 ? String8{v.keyspace.c_str, v.keyspace.length} : default_ks;
                if (ks_name.length == 0) {
                    return Result<type::Type>{{}, Error::MissingKeyspace, "no keyspace for UDT reference"};
                }
                auto ks_res = read_keyspace(schema, ks_name);
                if (ks_res.error != Error::None) {
                    return Result<type::Type>{{}, Error::MissingKeyspace, "UDT references unknown keyspace"};
                }
                String8           nm{v.name.c_str, v.name.length};
                type::UDT* const* p = find(ks_res.value->udts_by_name, nm);
                if (p == nullptr) {
                    return Result<type::Type>{{}, Error::MissingType, "user-defined type does not exist"};
                }
                return Result<type::Type>{type::create_udt(*p)};
            }
        });
    }
}
