// scriptingoptions_portable.cpp
//
// Portable (non-Windows) definition of OptionsTable[] and AlphaOptionsTable[].
// On Windows these live in scriptingoptions.cpp (compiled only via the .vcxproj).
// Here we provide the same tables using only portable headers so that
// GetOption / SetOption / GetDefaultValue / GetOptionList work on macOS/Linux.
//
// Guard: only compiled on non-Windows.

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "OtherTypes.h"

#include <stddef.h>   // offsetof
#include <climits>    // LONG_MAX, USHRT_MAX

// Silence the stub definitions in doc_portable.cpp — we provide the real ones.
// (The stubs in doc_portable.cpp are guarded so they don't conflict.)

// In the portable build we only need pName and iDefault from OptionsTable;
// offset and length are irrelevant since we never call SetOption portably.
#undef O
#undef A
#define O(arg) 0, 0
#define A(arg) 0

//-----config name ----------------  default ---variable name ---- min, max, flags ---
tConfigurationNumericOption OptionsTable [] = {
#include "scriptingoptions_numeric.inc"
{NULL}   // end of table marker            

  };  // end of OptionsTable 

// A(arg) intentionally kept as 0 in portable build — see line 26
                    
// first  optional argument: multiline flag
// second optional argument: preserve spaces flag (defaults to true for multiline)

#define MC_OUTPUT_FONT_DEFAULT ""
tConfigurationAlphaOption AlphaOptionsTable  [] =
{
#include "scriptingoptions_alpha.inc"
{NULL}  // end of table marker
};

#endif // !_WIN32
