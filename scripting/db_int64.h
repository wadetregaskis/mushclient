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
//   * a Lua number is a double, exact only to 2^53.
//
// So the column value is banded, and the SAME banding is used on both the
// VARIANT and the Lua paths (see DatabaseIntegerBand below):
//
//   |v| within 32-bit signed range  -> the integral form (VT_I4 / Lua number).
//                                     Unchanged from what scripts see today.
//   |v| <= 2^53                     -> a double (VT_R8 / Lua number), which
//                                     represents it exactly.
//   anything larger                 -> a decimal STRING (BSTR / Lua string),
//                                     which is lossless and which a script can
//                                     detect (type(v) == "string").
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
  eDatabaseIntegerAsDouble,   // exactly representable as a double
  eDatabaseIntegerAsString    // needs the full 64 bits: decimal string
  };

// Band boundaries, named rather than open-coded at the comparisons.
const sqlite3_int64 DATABASE_INTEGER_MIN_LONG      = -2147483647LL - 1LL;  // INT32_MIN
const sqlite3_int64 DATABASE_INTEGER_MAX_LONG      =  2147483647LL;        // INT32_MAX
const sqlite3_int64 DATABASE_INTEGER_EXACT_DOUBLE  =  9007199254740992LL;  // 2^53

inline eDatabaseIntegerBand DatabaseIntegerBand (const sqlite3_int64 iValue)
  {
  if (iValue >= DATABASE_INTEGER_MIN_LONG && iValue <= DATABASE_INTEGER_MAX_LONG)
    return eDatabaseIntegerAsLong;

  // 2^53 itself is exact; 2^53 + 1 is not.
  if (iValue >= -DATABASE_INTEGER_EXACT_DOUBLE && iValue <= DATABASE_INTEGER_EXACT_DOUBLE)
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
  strResult.Format ("%I64d", iValue);
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
