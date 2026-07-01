// speedwalk.h — shared speedwalk expansion/reversal.
//
// The speedwalk parser is pure string logic (no OS or GUI dependency). It is
// compiled into BOTH mushcore (the headless test engine) and mushapp (mushgui) from the
// single translation unit speedwalk.cpp, so the two builds can never drift.
//
// Direction resolution is injected via function pointers so the shared code
// never depends on GUI-only globals: the full build passes MapDirectionsMap-
// backed lookups; the portable build passes its built-in n/s/e/w/u/d tables.
//
// NB: include this AFTER the header that defines CString (doc.h / doc_stub.h).

#pragma once

// Forward expansion. `filler` is the world's speed-walk filler string (sent for
// an 'F' code); `endline` terminates each emitted command; `lookupDir` maps a
// lowercased cardinal-direction letter to the command to send ("" if unknown).
// On a malformed input, returns a string whose first character is '*'.
CString ExpandSpeedwalk (const char* input,
                         const CString& filler,
                         const char* endline,
                         CString (*lookupDir)(char letter));

// Reverse expansion (produces a re-speedwalkable path that is the inverse of the
// input). `lookupRev` maps a lowercased direction letter to its reverse letter;
// `lookupRevWord` maps a whole custom "(word)" to its reverse ("" if unknown).
// On a malformed input, returns a string whose first character is '*'.
CString ReverseSpeedwalkString (const char* input,
                                const char* endline,
                                CString (*lookupRev)(char letter),
                                CString (*lookupRevWord)(const CString& word));
