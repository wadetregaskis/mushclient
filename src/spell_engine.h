// spell_engine.h -- the operating system's spell checker, behind MUSHclient's.
//
// MUSHclient's spell checker (spellchecker.lua, its dialog and the script
// functions SpellCheck, SpellCheckDlg and AddSpellCheckWord) used only its
// own English word lists.  Where the operating system has a spell checker,
// that now decides which words are right and what to suggest instead, in
// whatever languages the user has, with the words they have taught it:
//
//   Windows   ISpellChecker (Windows 8 and later), in the user's language
//             spell_engine_win.cpp
//   macOS     NSSpellChecker, its languages as the user has set them
//             platform/spell_engine_mac.mm
//   Linux     Enchant (Hunspell, usually), in the language of $LANG, if
//             libenchant-2 is installed -- loaded when there, not linked
//             platform/spell_engine_enchant.cpp
//
// With none, the built-in word lists are used as before.  Either way,
// MUSHclient keeps a dictionary of its own (userdict.txt) for words the user
// would rather not teach every other program -- a MUD's names and jargon --
// and the dialog offers both: "Add to MUSHclient" and "Add to System".
//
// The engine is installed at startup by the platform's own code; the shared
// code (lua_utils.cpp's utils.spell_*) only asks for it, so the engine library
// and the test programs need no spell checker at all.

#pragma once

#include <string>
#include <vector>

class ISpellEngine
  {
  public:
    virtual ~ISpellEngine () {}

    // What it is, for a script or a message: "Windows (en-AU)".
    virtual std::string Name () = 0;

    // Words in the encoding the client uses: the ANSI code page on Windows,
    // UTF-8 elsewhere.
    virtual bool Check (const std::string & word) = 0;
    virtual std::vector<std::string> Suggest (const std::string & word) = 0;

    // Teach it a word, in the user's system-wide dictionary.
    virtual bool AddWord (const std::string & word) = 0;
  };

// The engine in use, or NULL for the built-in word lists.
ISpellEngine * SystemSpellEngine ();

// Install one (taking ownership); NULL removes it.
void SetSystemSpellEngine (ISpellEngine * engine);

// The platform's own engine, or NULL if it has none here (no Windows 8 spell
// checker, no libenchant, no dictionary for the language).  Defined by the
// platform's spell_engine_*.
ISpellEngine * CreatePlatformSpellEngine ();
