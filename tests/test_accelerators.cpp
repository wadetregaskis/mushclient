// tests/test_accelerators.cpp
// Tests for the world.Accelerator / AcceleratorTo / AcceleratorList API.
//
// Why this file exists: on the port these three were dead.  AcceleratorTo
// opened with a walk of the doc's view list to find the MDI child frame that
// owns the Win32 HACCEL table, and bailed out with eBadParameter when the walk
// found nothing.  CMUSHView is never constructed off-Windows, so the walk found
// nothing every single time and no script could ever bind a key.  The HACCEL
// machinery is now Windows-only and the map bookkeeping — which is what
// CWorldChildFrame::FireAcceleratorIfMatch actually dispatches from — runs
// everywhere.
//
// The three maps are the observable:
//   m_AcceleratorToCommandMap   (fVirt<<16 | key) → command id
//   m_CommandToMacroMap         command id       → text to send
//   m_CommandToSendToMap        command id       → eSendTo* destination

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"   // ACCELERATOR_FIRST_COMMAND / ACCELERATOR_COUNT (the id pool)
#include "scripting/errors.h"
#include "variant_test_helpers.h"

#include <string>

// ── helpers ───────────────────────────────────────────────────────────────

namespace {

// The accelerator map is keyed on the modifier byte OR'd with FNOINVERT (the
// caller's fVirt always gains it) shifted left 16, plus the virtual-key code.
long key_of(BYTE fVirt, WORD key)
{
    return ((long) (fVirt | FNOINVERT)) << 16 | key;
}

// Look up the command id bound to a keystroke, or -1 if it is unbound.
long command_for(CMUSHclientDoc& doc, BYTE fVirt, WORD key)
{
    map<long, WORD>::const_iterator it =
        doc.m_AcceleratorToCommandMap.find(key_of(fVirt, key));
    return it == doc.m_AcceleratorToCommandMap.end() ? -1 : (long) it->second;
}

}   // namespace

// ── AcceleratorTo: the registration that used to be unreachable ────────────

// The headline regression: binding a key must succeed and must land in the
// three maps.  Before the fix this returned eBadParameter and touched nothing.
TEST(Accelerators, AcceleratorToRegistersTheKeystroke)
{
    CMUSHclientDoc doc;

    EXPECT_EQ((long) eOK, doc.AcceleratorTo("F5", "sing", eSendToWorld));

    const long cmd = command_for(doc, FVIRTKEY, VK_F5);
    ASSERT_NE(-1L, cmd);
    EXPECT_EQ(std::string("sing"), doc.m_CommandToMacroMap[(WORD) cmd]);
    EXPECT_EQ((int) eSendToWorld, (int) doc.m_CommandToSendToMap[(WORD) cmd]);
}

// Modifiers have to survive the round trip, or Ctrl+F5 would collide with F5.
TEST(Accelerators, ModifiersAreDistinctKeystrokes)
{
    CMUSHclientDoc doc;

    EXPECT_EQ((long) eOK, doc.AcceleratorTo("F5", "plain", eSendToWorld));
    EXPECT_EQ((long) eOK, doc.AcceleratorTo("Ctrl+F5", "ctrl", eSendToWorld));
    EXPECT_EQ((long) eOK, doc.AcceleratorTo("Shift+Alt+F5", "shiftalt", eSendToWorld));

    const long plain    = command_for(doc, FVIRTKEY, VK_F5);
    const long ctrl     = command_for(doc, FVIRTKEY | FCONTROL, VK_F5);
    const long shiftalt = command_for(doc, FVIRTKEY | FSHIFT | FALT, VK_F5);

    ASSERT_NE(-1L, plain);
    ASSERT_NE(-1L, ctrl);
    ASSERT_NE(-1L, shiftalt);
    EXPECT_NE(plain, ctrl);
    EXPECT_NE(plain, shiftalt);
    EXPECT_NE(ctrl, shiftalt);

    EXPECT_EQ(std::string("plain"),    doc.m_CommandToMacroMap[(WORD) plain]);
    EXPECT_EQ(std::string("ctrl"),     doc.m_CommandToMacroMap[(WORD) ctrl]);
    EXPECT_EQ(std::string("shiftalt"), doc.m_CommandToMacroMap[(WORD) shiftalt]);
}

// Re-binding a key keeps the same command id (Windows re-uses it rather than
// leaking one out of the fixed ACCELERATOR_COUNT pool) and replaces the text.
TEST(Accelerators, RebindingAKeyReusesTheCommandId)
{
    CMUSHclientDoc doc;

    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "first", eSendToWorld));
    const long first = command_for(doc, FVIRTKEY, VK_F5);

    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "second", eSendToExecute));
    const long second = command_for(doc, FVIRTKEY, VK_F5);

    EXPECT_EQ(first, second);
    EXPECT_EQ(std::string("second"), doc.m_CommandToMacroMap[(WORD) second]);
    EXPECT_EQ((int) eSendToExecute, (int) doc.m_CommandToSendToMap[(WORD) second]);
    EXPECT_EQ(1u, doc.m_AcceleratorToCommandMap.size());
}

// An empty Send string means "forget this key" (new in MUSHclient 3.75).  On
// Windows that fell out of rebuilding the HACCEL table without the entry; off
// Windows the map IS the table, so the erase has to be explicit.
TEST(Accelerators, EmptySendUnregistersTheKeystroke)
{
    CMUSHclientDoc doc;

    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "sing", eSendToWorld));
    const long cmd = command_for(doc, FVIRTKEY, VK_F5);
    ASSERT_NE(-1L, cmd);

    EXPECT_EQ((long) eOK, doc.AcceleratorTo("F5", "", eSendToWorld));

    EXPECT_EQ(-1L, command_for(doc, FVIRTKEY, VK_F5));
    EXPECT_TRUE(doc.m_CommandToMacroMap.find((WORD) cmd) == doc.m_CommandToMacroMap.end());
    EXPECT_TRUE(doc.m_CommandToSendToMap.find((WORD) cmd) == doc.m_CommandToSendToMap.end());
}

// ...and un-binding must not COST an id, which it did until the free list.
//
// RebindingAKeyReusesTheCommandId above says a re-bind "re-uses it rather than
// leaking one out of the fixed ACCELERATOR_COUNT pool" — but it never unbinds
// in between, so it only ever exercises the map-HIT branch and passed either
// way.  Once the key has been forgotten the lookup misses, and the allocator
// used to run straight to `m_nextAcceleratorCommand++`.  Nothing ever returned
// an id — the counter is seeded once in doc_construct.cpp and only incremented
// — so each unbind/rebind cycle consumed one of the 1000 permanently
// (ACCELERATOR_FIRST_COMMAND 12000 .. +ACCELERATOR_COUNT, mainfrm.h), and at
// exhaustion AcceleratorTo returned eBadParameter for EVERY key in that world.
//
// The erase itself is not the problem and cannot simply be dropped:
// AcceleratorList enumerates m_AcceleratorToCommandMap, so hiding an un-bound
// key from the list requires destroying the entry that would have supplied the
// old id.  Releasing the id to a free list buys both, and restores upstream's
// property that binding the same key twice costs one id — which upstream got
// for free by never erasing, at the price of listing dead keys forever.
TEST(Accelerators, UnbindingReleasesTheCommandIdForReuse)
{
    CMUSHclientDoc doc;

    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "first", eSendToWorld));
    const long before = command_for(doc, FVIRTKEY, VK_F5);
    ASSERT_NE(-1L, before);

    // forget it, then bind the SAME keystroke again
    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "", eSendToWorld));
    ASSERT_EQ(-1L, command_for(doc, FVIRTKEY, VK_F5));
    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "second", eSendToWorld));

    const long after = command_for(doc, FVIRTKEY, VK_F5);
    ASSERT_NE(-1L, after);

    EXPECT_EQ(before, after);   // the released id came back
    // the binding itself is otherwise intact
    EXPECT_EQ(std::string("second"), doc.m_CommandToMacroMap[(WORD) after]);
    EXPECT_EQ(1u, doc.m_AcceleratorToCommandMap.size());
}

// The pool survives a great many cycles — the property the free list exists
// for.  Without it this exhausts ACCELERATOR_COUNT and starts failing; a plain
// "it worked twice" test would not notice, because the leak is one per cycle.
TEST(Accelerators, ManyUnbindRebindCyclesDoNotExhaustThePool)
{
    CMUSHclientDoc doc;

    // one cycle per id in the pool, plus a margin: without the free list this
    // starts returning eBadParameter partway through
    int failedAt = -1;
    for (int i = 0; i < ACCELERATOR_COUNT + 50 && failedAt < 0; i++) {
        if (doc.AcceleratorTo("F5", "x", eSendToWorld) != (long) eOK ||
            doc.AcceleratorTo("F5", "", eSendToWorld)  != (long) eOK)
            failedAt = i;
    }
    EXPECT_EQ(-1, failedAt);   // -1 = never failed; otherwise the cycle number
    // and a different key can still be bound afterwards
    EXPECT_EQ((long) eOK, doc.AcceleratorTo("F6", "still works", eSendToWorld));

    // Distinct keys bound SIMULTANEOUSLY still each cost one, so the free list
    // has not turned the pool into an unbounded resource.
    CMUSHclientDoc doc2;
    ASSERT_EQ((long) eOK, doc2.AcceleratorTo("F5", "a", eSendToWorld));
    ASSERT_EQ((long) eOK, doc2.AcceleratorTo("F6", "b", eSendToWorld));
    EXPECT_NE(command_for(doc2, FVIRTKEY, VK_F5),
              command_for(doc2, FVIRTKEY, VK_F6));
}

// Deleting one keystroke must not disturb its neighbours.
TEST(Accelerators, EmptySendLeavesOtherKeystrokesAlone)
{
    CMUSHclientDoc doc;

    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "five", eSendToWorld));
    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F6", "six",  eSendToWorld));

    ASSERT_EQ((long) eOK, doc.AcceleratorTo("F5", "", eSendToWorld));

    EXPECT_EQ(-1L, command_for(doc, FVIRTKEY, VK_F5));
    const long six = command_for(doc, FVIRTKEY, VK_F6);
    ASSERT_NE(-1L, six);
    EXPECT_EQ(std::string("six"), doc.m_CommandToMacroMap[(WORD) six]);
}

// Deleting a key that was never bound is a no-op, not an error or a crash.
TEST(Accelerators, EmptySendOnAnUnboundKeyIsHarmless)
{
    CMUSHclientDoc doc;

    EXPECT_EQ((long) eOK, doc.AcceleratorTo("F9", "", eSendToWorld));
    EXPECT_TRUE(doc.m_AcceleratorToCommandMap.empty());
}

// ── argument validation ───────────────────────────────────────────────────

TEST(Accelerators, SendToOutOfRangeIsRejected)
{
    CMUSHclientDoc doc;

    EXPECT_EQ((long) eOptionOutOfRange, doc.AcceleratorTo("F5", "sing", -1));
    EXPECT_EQ((long) eOptionOutOfRange, doc.AcceleratorTo("F5", "sing", eSendToLast));
    EXPECT_TRUE(doc.m_AcceleratorToCommandMap.empty());
}

// StringToKeyCode throws on an unparseable key name; AcceleratorTo turns that
// into eBadParameter rather than letting it escape into the script engine.
TEST(Accelerators, UnknownKeyNameIsBadParameter)
{
    CMUSHclientDoc doc;

    EXPECT_EQ((long) eBadParameter, doc.AcceleratorTo("Ctrl+Nonesuch", "sing", eSendToWorld));
    EXPECT_TRUE(doc.m_AcceleratorToCommandMap.empty());
}

// ── Accelerator (the two-argument form) ───────────────────────────────────

// Accelerator is AcceleratorTo with eSendToExecute — the historical default,
// which is also the destination AcceleratorList renders as blank.
TEST(Accelerators, AcceleratorDefaultsToSendToExecute)
{
    CMUSHclientDoc doc;

    EXPECT_EQ((long) eOK, doc.Accelerator("Ctrl+F8", "hello"));

    const long cmd = command_for(doc, FVIRTKEY | FCONTROL, VK_F8);
    ASSERT_NE(-1L, cmd);
    EXPECT_EQ(std::string("hello"), doc.m_CommandToMacroMap[(WORD) cmd]);
    EXPECT_EQ((int) eSendToExecute, (int) doc.m_CommandToSendToMap[(WORD) cmd]);
}

// ── AcceleratorList ───────────────────────────────────────────────────────

// MFC parity: a COleSafeArray that was never created detaches as VT_EMPTY.
TEST(Accelerators, AcceleratorListIsEmptyWhenNothingIsBound)
{
    CMUSHclientDoc doc;

    VARIANT v = doc.AcceleratorList();
    EXPECT_EQ((int) VT_EMPTY, (int) v.vt);
}

// Each entry renders as "<key> = <text>", with a "\t[n]" destination suffix for
// everything except eSendToExecute, which stays blank for backwards
// compatibility with scripts written before AcceleratorTo existed.
TEST(Accelerators, AcceleratorListRendersKeyTextAndDestination)
{
    CMUSHclientDoc doc;

    ASSERT_EQ((long) eOK, doc.Accelerator("F5", "sing"));                  // eSendToExecute
    ASSERT_EQ((long) eOK, doc.AcceleratorTo("Ctrl+F6", "dance", eSendToWorld));

    VARIANT v = doc.AcceleratorList();
    ASSERT_EQ((int) (VT_ARRAY | VT_VARIANT), (int) v.vt);
    COleSafeArray sa;
    sa.Attach(v);
    ASSERT_EQ(2L, sa.GetOneDimSize());

    bool sawExecute = false, sawWorld = false;
    for (long i = 0; i < sa.GetOneDimSize(); i++) {
        VARIANT e; VariantInit(&e);
        sa.GetElement(&i, &e);
        const std::string s = vs(e);
        if (s == "F5 = sing")
            sawExecute = true;
        if (s == "Ctrl+F6 = dance\t[" + std::to_string((int) eSendToWorld) + "]")
            sawWorld = true;
    }
    EXPECT_TRUE(sawExecute);
    EXPECT_TRUE(sawWorld);
}

// A key deleted with an empty Send must disappear from the list too — the
// listing reads m_AcceleratorToCommandMap, so this is the end-to-end check
// that the off-Windows delete arm really unhooks the entry.
TEST(Accelerators, AcceleratorListDropsDeletedKeystrokes)
{
    CMUSHclientDoc doc;

    ASSERT_EQ((long) eOK, doc.Accelerator("F5", "sing"));
    ASSERT_EQ((long) eOK, doc.Accelerator("F6", "dance"));
    ASSERT_EQ((long) eOK, doc.Accelerator("F5", ""));

    VARIANT v = doc.AcceleratorList();
    ASSERT_EQ((int) (VT_ARRAY | VT_VARIANT), (int) v.vt);
    COleSafeArray sa;
    sa.Attach(v);
    ASSERT_EQ(1L, sa.GetOneDimSize());

    long i = 0;
    VARIANT e; VariantInit(&e);
    sa.GetElement(&i, &e);
    EXPECT_EQ(std::string("F6 = dance"), vs(e));
}

// ── KeyCodeToString / StringToKeyCode round trip ──────────────────────────
//
// KeyCodeToString moved out of Utilities.cpp (mushgui-only) into
// utilities_shared.cpp so the headless engine could link AcceleratorList.  Pin
// its output, since AcceleratorList's rendering is now the only thing that
// would notice if the move went wrong.
TEST(Accelerators, KeyCodeToStringRoundTripsThroughStringToKeyCode)
{
    static const char* const kKeys[] = {
        "F1", "Ctrl+F5", "Shift+F12", "Alt+Home", "Ctrl+Alt+Delete",
        "Shift+Ctrl+Alt+Insert", "Ctrl+PageUp", "Esc", "Alt+Space", "Tab",
    };

    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++) {
        BYTE fVirt = 0;
        WORD key   = 0;
        StringToKeyCode(kKeys[i], fVirt, key);
        // (mc_test's EXPECT_EQ has no <<-message form; the key is in the value.)
        EXPECT_EQ(std::string(kKeys[i]),
                  std::string((LPCTSTR) KeyCodeToString(fVirt, key)));
    }
}

// The parse really does throw on nonsense — the headless build used to link a
// stub that quietly handed back 0/0, which is what made every accelerator test
// pass before this file existed.
TEST(Accelerators, StringToKeyCodeThrowsOnAnUnknownKeyName)
{
    BYTE fVirt = 0;
    WORD key   = 0;
    bool threw = false;
    try {
        StringToKeyCode("Nonesuch", fVirt, key);
    } catch (CException* e) {
        e->Delete();
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// Two key names in one accelerator is also rejected.
TEST(Accelerators, StringToKeyCodeRejectsTwoKeystrokes)
{
    BYTE fVirt = 0;
    WORD key   = 0;
    bool threw = false;
    try {
        StringToKeyCode("F5+F6", fVirt, key);
    } catch (CException* e) {
        e->Delete();
        threw = true;
    }
    EXPECT_TRUE(threw);
}

MC_TEST_MAIN()
