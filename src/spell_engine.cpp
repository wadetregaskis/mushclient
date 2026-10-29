// spell_engine.cpp -- the one engine in use.  See spell_engine.h.

#include "stdafx.h"
#include "spell_engine.h"

#include <memory>

namespace {

std::unique_ptr<ISpellEngine> & Engine ()
  {
  static std::unique_ptr<ISpellEngine> engine;
  return engine;
  }

}   // namespace

ISpellEngine * SystemSpellEngine ()
  {
  return Engine ().get ();
  }

void SetSystemSpellEngine (ISpellEngine * engine)
  {
  Engine ().reset (engine);
  }
