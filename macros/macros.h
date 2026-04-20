#pragma once

// clang OS/arch cracking

#if defined(__clang__)

    #define PLEXDB_COMPILER_CLANG 1

    #if defined(_WIN32)
        #define PLEXDB_OS_WINDOWS 1
    #elif defined(__gnu_linux__) || defined(__linux__)
        #define PLEXDB_OS_LINUX 1
    #elif defined(__APPLE__) && defined(__MACH__)
        #define PLEXDB_OS_MAC 1
    #elif defined(__EMSCRIPTEN__)
        #define PLEXDB_OS_WEB 1
    #else
        #error This compiler/OS combo is not supported.
    #endif

    #if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64) || defined(__wasm64__)
        #define PLEXDB_ARCH_X64 1
    #elif defined(i386) || defined(__i386) || defined(__i386__) || defined(__wasm32__)
        #define PLEXDB_ARCH_X86 1
    #elif defined(__aarch64__)
        #define PLEXDB_ARCH_ARM64 1
    #elif defined(__arm__)
        #define PLEXDB_ARCH_ARM32 1
    #else
        #error Architecture not supported.
    #endif

// MSVC OS/arch cracking

#elif defined(_MSC_VER)

    #define PLEXDB_COMPILER_MSVC 1

    #if _MSC_VER >= 1920
        #define PLEXDB_COMPILER_MSVC_YEAR 2019
    #elif _MSC_VER >= 1910
        #define PLEXDB_COMPILER_MSVC_YEAR 2017
    #elif _MSC_VER >= 1900
        #define PLEXDB_COMPILER_MSVC_YEAR 2015
    #elif _MSC_VER >= 1800
        #define PLEXDB_COMPILER_MSVC_YEAR 2013
    #elif _MSC_VER >= 1700
        #define PLEXDB_COMPILER_MSVC_YEAR 2012
    #elif _MSC_VER >= 1600
        #define PLEXDB_COMPILER_MSVC_YEAR 2010
    #elif _MSC_VER >= 1500
        #define PLEXDB_COMPILER_MSVC_YEAR 2008
    #elif _MSC_VER >= 1400
        #define PLEXDB_COMPILER_MSVC_YEAR 2005
    #else
        #define PLEXDB_COMPILER_MSVC_YEAR 0
    #endif

    #if defined(_WIN32)
        #define PLEXDB_OS_WINDOWS 1
    #else
        #error This compiler/OS combo is not supported.
    #endif

    #if defined(_M_AMD64)
        #define PLEXDB_ARCH_X64 1
    #elif defined(_M_IX86)
        #define PLEXDB_ARCH_X86 1
    #elif defined(_M_ARM64)
        #define PLEXDB_ARCH_ARM64 1
    #elif defined(_M_ARM)
        #define PLEXDB_ARCH_ARM32 1
    #else
        #error Architecture not supported.
    #endif

// GCC OS/arch cracking

#elif defined(__GNUC__) || defined(__GNUG__)

    #define PLEXDB_COMPILER_GCC 1

    #if defined(__gnu_linux__) || defined(__linux__)
        #define PLEXDB_OS_LINUX 1
    #else
        #error This compiler/OS combo is not supported.
    #endif

    #if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
        #define PLEXDB_ARCH_X64 1
    #elif defined(i386) || defined(__i386) || defined(__i386__)
        #define PLEXDB_ARCH_X86 1
    #elif defined(__aarch64__)
        #define PLEXDB_ARCH_ARM64 1
    #elif defined(__arm__)
        #define PLEXDB_ARCH_ARM32 1
    #else
        #error Architecture not supported.
    #endif

#else
    #error Compiler not supported.
#endif

// arch cracking

#if defined(PLEXDB_ARCH_X64) || defined(PLEXDB_ARCH_ARM64)
    #define PLEXDB_ARCH_64BIT 1
#elif defined(PLEXDB_ARCH_X86) || defined(PLEXDB_ARCH_ARM32)
    #define PLEXDB_ARCH_32BIT 1
#endif

#if PLEXDB_ARCH_ARM32 || PLEXDB_ARCH_ARM64 || PLEXDB_ARCH_X64 || PLEXDB_ARCH_X86
    #define PLEXDB_ARCH_LITTLE_ENDIAN 1
#else
    #error Endianness of this architecture could not be deduced.
#endif

// language cracking

#if defined(__cplusplus)
    #define PLEXDB_LANG_CPP 1
#else
    #define PLEXDB_LANG_C 1
#endif

// utilities

#if !defined(PLEXDB_DEBUG)
    #define PLEXDB_DEBUG 0
#endif
#if PLEXDB_DEBUG
    #define PLEXDB_DEBUG_X(x) x
#else
    #define PLEXDB_DEBUG_X(x)
#endif

#if PLEXDB_COMPILER_GCC || PLEXDB_COMPILER_CLANG
    #define PLEXDB_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define PLEXDB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #error Likely/unlikely not implemented.
#endif

#if PLEXDB_COMPILER_MSVC
    #define PLEXDB_TRAP __debugbreak();
#elif PLEXDB_COMPILER_GCC || PLEXDB_COMPILER_CLANG
    #define PLEXDB_TRAP __builtin_trap();
#else
    #error Trap not implemented.
#endif

// zero undefined options

#if !defined(PLEXDB_ARCH_LITTLE_ENDIAN)
    #define PLEXDB_ARCH_LITTLE_ENDIAN 0
#endif
#if !defined(PLEXDB_ARCH_BIG_ENDIAN)
    #define PLEXDB_ARCH_BIG_ENDIAN 0
#endif
#if !defined(PLEXDB_ARCH_32BIT)
    #define PLEXDB_ARCH_32BIT 0
#endif
#if !defined(PLEXDB_ARCH_64BIT)
    #define PLEXDB_ARCH_64BIT 0
#endif
#if !defined(PLEXDB_ARCH_X64)
    #define PLEXDB_ARCH_X64 0
#endif
#if !defined(PLEXDB_ARCH_X86)
    #define PLEXDB_ARCH_X86 0
#endif
#if !defined(PLEXDB_ARCH_ARM64)
    #define PLEXDB_ARCH_ARM64 0
#endif
#if !defined(PLEXDB_ARCH_ARM32)
    #define PLEXDB_ARCH_ARM32 0
#endif
#if !defined(PLEXDB_COMPILER_MSVC)
    #define PLEXDB_COMPILER_MSVC 0
#endif
#if !defined(PLEXDB_COMPILER_GCC)
    #define PLEXDB_COMPILER_GCC 0
#endif
#if !defined(PLEXDB_COMPILER_CLANG)
    #define PLEXDB_COMPILER_CLANG 0
#endif
#if !defined(PLEXDB_OS_WINDOWS)
    #define PLEXDB_OS_WINDOWS 0
#endif
#if !defined(PLEXDB_OS_LINUX)
    #define PLEXDB_OS_LINUX 0
#endif
#if !defined(PLEXDB_OS_WEB)
    #define PLEXDB_OS_WEB 0
#endif
#if !defined(PLEXDB_OS_MAC)
    #define PLEXDB_OS_MAC 0
#endif
#if !defined(LANG_CPP)
    #define LANG_CPP 0
#endif
#if !defined(LANG_C)
    #define LANG_C 0
#endif

#if PLEXDB_COMPILER_MSVC
    #define thread_static __declspec(thread)
#elif PLEXDB_COMPILER_CLANG
    #define thread_static __thread
#elif PLEXDB_COMPILER_GCC
    #define thread_static __thread
#else
    #error Thread static not defined for this compiler.
#endif
#if PLEXDB_DISABLE_THREADS
    #undef thread_static
    #define thread_static
#endif

#define PLEXDB_CONSTEVAL_TRAP(x) (void)(1 / static_cast<int>(static_cast<bool>(x)))

// OffsetOf
#include <cstddef>

// fixed type format specifier
#include <inttypes.h>