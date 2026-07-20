// tests/variant_test_helpers.h
// Shared VARIANT/BSTR extraction helper (audit §2/§4): the identical
//   static std::string vs(const VARIANT&)
// was redefined verbatim in test_triggers.cpp, test_aliases.cpp and
// test_timers.cpp.  Hoisted here to a single source of truth.
//
// The including TU must already have pulled in the MFC/Win32 shim (VARIANT,
// CString) and <string>.

#ifndef MUSHCLIENT_TESTS_VARIANT_TEST_HELPERS_H
#define MUSHCLIENT_TESTS_VARIANT_TEST_HELPERS_H

#include <string>

// Extract a VARIANT's BSTR payload as a std::string (or a sentinel if it is
// not a string VARIANT).  static inline: an unused copy raises no warning.
static inline std::string vs(const VARIANT& v)
{
    if (v.vt != VT_BSTR) return "<not-a-string>";
    return std::string((const char*)CString(v.bstrVal));
}

#endif // MUSHCLIENT_TESTS_VARIANT_TEST_HELPERS_H
