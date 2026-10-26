// option_table_offsetof.h -- include in a TU whose option tables use offsetof.
//
// The scripting option tables (world, global, trigger, alias, timer and chat
// options) record each setting as offsetof (Class, member).  Those classes have
// virtual functions, so they are not standard-layout, and offsetof on them is
// "conditionally-supported" (C++17 [support.types.layout]) -- which clang and
// GCC report as -Winvalid-offsetof.  Every compiler this project builds with
// (MSVC, GCC, clang) supports it for a member reached without a virtual base,
// and none of these classes has one; clang and GCC make offsetof through a
// virtual base a hard error, so that precondition is enforced by the compiler
// rather than assumed.  Rewriting ~400 table rows as pointers-to-member would
// buy nothing, so the warning is silenced here, for these TUs only.
//
// MSVC does not have the warning, and would warn C4068 about the unknown pragma.

#ifndef MUSHCLIENT_OPTION_TABLE_OFFSETOF_H
#define MUSHCLIENT_OPTION_TABLE_OFFSETOF_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#endif // MUSHCLIENT_OPTION_TABLE_OFFSETOF_H
