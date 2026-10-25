// pal/iclock.h — the injectable wall-clock time source of the Platform
// Abstraction Layer (design: docs/PAL.md; one of the eight PAL interfaces).
//
// MUSHclient's timer/uptime/duration logic all funnels through a single time
// read, CmcDateTime::GetTimeNow().  IClock lets that read be overridden so the
// timer subsystem can be driven deterministically (advance a fake clock, run
// CheckTimerList, assert firing — no wall-clock sleeps).
//
// The default is "no clock installed" (g_clock == nullptr), which means
// GetTimeNow() reads the real system clock exactly as it always has — so
// production behaviour on every platform is byte-for-byte unchanged, and only
// tests (or a future fully-virtual-time frontend) install an adapter.
//
// This header is DELIBERATELY dependency-free: an abstract interface plus one
// extern pointer, no includes.  It is a contract, not a layer — so the
// foundation datetime code consulting it is not a dependency inversion.  The
// clock currency is the OLE automation date (days since 1899-12-30, fractional
// part = time of day) that CmcDateTime already wraps, kept as a bare double so
// this header need not pull in mcdatetime.h.

#pragma once

namespace pal {

class IClock {
public:
    virtual ~IClock() = default;

    // "Now" as an OLE automation date double (the value CmcDateTime stores in
    // m_dt).  Called on the main thread from CmcDateTime::GetTimeNow().
    virtual double nowOle() const = 0;
};

// The installed clock, or nullptr = read the real system clock (the default,
// and the only state in production).  Defined in mcdatetime.cpp so every build
// that has the datetime foundation links it.
extern IClock* g_clock;

} // namespace pal
