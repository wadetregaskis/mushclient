// scripting/db_int64.h
//
// How an SQLite INTEGER column value reaches a script.
//
// SQLite stores INTEGER as a signed 64-bit value, but every representation
// MUSHclient can hand a script is narrower than that:
//
//   * the automation VARIANT (Python/Perl/JavaScript/Tcl/Ruby, via the
//     dispatch map) has no 64-bit integer type -- VT_I8 does not exist in
//     the portable substrate, and never existed in the shipping dispatch
//     surface either; the integral type is VT_I4, a 32-bit long;
//   * a Lua number is a double, which is exact for *consecutive* integers only
//     up to 2^53.  Past that it still holds a great many individual values
//     exactly -- every even number, then every multiple of 4, and so on -- it
//     simply cannot hold all of them.
//
// So the column value is banded, and the SAME banding is used on both the
// VARIANT and the Lua paths (see DatabaseIntegerBand below):
//
//   within the 32-bit signed range -> the integral form (VT_I4 / Lua number).
//                                     Unchanged from what scripts see today.
//   survives a round trip through  -> a double (VT_R8 / Lua number), which
//     a double                        therefore represents it exactly.
//   anything else                  -> a decimal STRING (BSTR / Lua string),
//                                     which is lossless and which a script can
//                                     detect (type(v) == "string").
//
// The middle band is a VALUE test, not a MAGNITUDE test, and that is the whole
// point of it.  MUSHclient ships two SQLite surfaces -- these four
// world.Database* readers and the bundled Lua `sqlite3` module -- and the
// module reached for the value test first, in vm_push_column and its two
// siblings (lsqlite/lsqlite3.c), originally written as:
//
//     lua_Number n = (lua_Number) i64;
//     if (n == i64) lua_pushnumber (L, n); else <push the decimal text>
//
// A magnitude band capped at 2^53 disagrees with that for every exactly
// representable value above the cap -- 2^60, say, which is a power of two and
// so is held perfectly by a double, yet which the old band stringified.  The
// round trip removes that disagreement.
//
// The two surfaces agreed completely as of fbb6695.  They did not at first:
// lsqlite3 spelled its round trip `n == i64`, where the usual arithmetic
// conversions widen the int64 operand to double before comparing, so the test
// read (double) i64 == (double) i64 and was vacuously true -- its string branch
// was unreachable and it pushed a silently rounded number for values a double
// cannot hold, where we return the lossless decimal string.  That was recorded
// here as a deliberate residual, on the grounds that changing vendored
// third-party behaviour is a separate decision; the decision was then taken and
// lsqlite3 fixed (docs/UPSTREAM-BUGS.md 87), so no residual remains.  Both sides
// are pinned by Lsqlite3ModuleBandsExactlyAsWeDo and
// Lsqlite3ModuleAgreesOnWhatADoubleCanHold in tests/test_database.cpp, so the two
// surfaces cannot drift apart unnoticed.  Keep them in step.
//
// The string band follows the precedent already set by DatabaseLastInsertRowid
// (methods_database.cpp), which has always returned an sqlite3_int64 rowid as a
// formatted string for exactly this reason.
//
// Before this banding existed all four column readers called sqlite3_column_int
// -- 32 bits -- so any wider value was silently truncated, in all six scripting
// languages.

#pragma once

// The three bands.  Every reader switches on this one function, so the two
// VARIANT readers and the two Lua readers cannot drift apart.
enum eDatabaseIntegerBand
  {
  eDatabaseIntegerAsLong,     // fits a 32-bit signed long
  eDatabaseIntegerAsDouble,   // a double holds it exactly
  eDatabaseIntegerAsString    // a double would round it: decimal string
  };

// Band boundaries, named rather than open-coded at the comparisons.
const sqlite3_int64 DATABASE_INTEGER_MIN_LONG = -2147483647LL - 1LL;  // INT32_MIN
const sqlite3_int64 DATABASE_INTEGER_MAX_LONG =  2147483647LL;        // INT32_MAX

// 2^63, itself exactly representable as a double, and the smallest positive
// double that is OUT of sqlite3_int64's range.  The round trip has to be
// guarded on the DOUBLE side by it, because (double) INT64_MAX rounds *up* to
// exactly this value and casting that back to an integer is undefined: arm64
// saturates to INT64_MAX, so an unguarded `(sqlite3_int64) (double) v == v`
// would claim INT64_MAX round-trips when it plainly does not, while x86-64
// yields INT64_MIN and would not.  Comparing before the cast removes the
// undefined behaviour and the platform dependency together.
const double DATABASE_INTEGER_TWO_POW_63 = 9223372036854775808.0;

inline eDatabaseIntegerBand DatabaseIntegerBand (const sqlite3_int64 iValue)
  {
  if (iValue >= DATABASE_INTEGER_MIN_LONG && iValue <= DATABASE_INTEGER_MAX_LONG)
    return eDatabaseIntegerAsLong;

  const double dValue = (double) iValue;

  // Out of range for the trip home -- INT64_MAX and its immediate neighbours.
  if (dValue < -DATABASE_INTEGER_TWO_POW_63 || dValue >= DATABASE_INTEGER_TWO_POW_63)
    return eDatabaseIntegerAsString;

  // The round trip proper.  Every 32-bit value passes it too, so the band
  // above is a strict subset of this one and the two can never conflict.
  if ((sqlite3_int64) dValue == iValue)
    return eDatabaseIntegerAsDouble;

  return eDatabaseIntegerAsString;
  }   // end of DatabaseIntegerBand

// The decimal rendering used by the string band.  "%I64d" is MSVC's 64-bit
// length modifier; CString::FormatV rewrites it to "ll" off Windows
// (platform/mfc_string.h), which is why DatabaseLastInsertRowid's use of it
// works on the port too.
inline CString DatabaseIntegerAsString (const sqlite3_int64 iValue)
  {
  CString strResult;
  strResult.Format ("%lld", (long long) iValue);
  return strResult;
  }   // end of DatabaseIntegerAsString

// The one int64 -> VARIANT decision, shared by DatabaseColumnValue and (for
// its two non-string bands) DatabaseColumnValues.  Mirrors the SetUpVariant*
// family in doc_shared.cpp: the BSTR is freshly allocated and owned by the
// caller.
inline void SetUpVariantDatabaseInteger (VARIANT & tVariant, const sqlite3_int64 iValue)
  {
  VariantClear (&tVariant);

  switch (DatabaseIntegerBand (iValue))
    {
    case eDatabaseIntegerAsLong:
      tVariant.vt = VT_I4;
      tVariant.lVal = (long) iValue;
      break;

    case eDatabaseIntegerAsDouble:
      tVariant.vt = VT_R8;
      tVariant.dblVal = (double) iValue;
      break;

    default:
      tVariant.vt = VT_BSTR;
      tVariant.bstrVal = DatabaseIntegerAsString (iValue).AllocSysString ();
      break;
    }
  }   // end of SetUpVariantDatabaseInteger
