// platform/mc_variant_args.h — a per-call VARIANT argument buffer for the five
// embedded engines' world.* call path.
//
// The engines marshalled their arguments into a `std::vector<VARIANT>`, which in
// libc++ (no small-buffer optimization) is one malloc + one free on EVERY call
// that carries an argument — and Note/Send/SetVariable/GetVariable, the bulk of a
// busy plugin's traffic, all carry at least one.  This holds the common small
// arity inline (no allocation at all) and falls back to the heap only beyond it;
// every slot is VariantInit'd on construction and MC_VariantClear'd on
// destruction, so it is a drop-in for the vector plus its manual clear loop, and
// it cleans up on any C++ exception between fill and use.
//
// Include AFTER the header that completes VARIANT (stdafx.h) and after
// mc_dispatch.h (for MC_VariantClear).  VARIANT is only forward-declared in
// mc_dispatch.h, which is why this cannot live there.

#pragma once

#include <cstdlib>

class MCVariantArgs
  {
  public:
    explicit MCVariantArgs (size_t count) : m_n (count)
      {
      m_p = (count <= kInline)
              ? m_inline
              : static_cast<VARIANT*> (malloc (count * sizeof (VARIANT)));
      for (size_t i = 0; i < count; ++i)
        VariantInit (&m_p[i]);
      }

    ~MCVariantArgs ()
      {
      for (size_t i = 0; i < m_n; ++i)
        MC_VariantClear (m_p[i]);
      if (m_p != m_inline)
        free (m_p);
      }

    MCVariantArgs (const MCVariantArgs&)            = delete;
    MCVariantArgs& operator= (const MCVariantArgs&) = delete;

    VARIANT& operator[] (size_t i) { return m_p[i]; }
    VARIANT* data ()               { return m_p; }
    // Iterable so the engines' existing `for (auto& a : vargs) MC_VariantClear(a)`
    // loops compile unchanged.  Clearing a slot the loop already cleared is a
    // safe no-op (var_clear leaves a VT_EMPTY variant untouched), so the loop and
    // the destructor never double-free -- the destructor's only extra duty is
    // freeing the heap buffer when the arity overflowed the inline storage.
    VARIANT* begin ()              { return m_p; }
    VARIANT* end   ()              { return m_p + m_n; }

  private:
    static const size_t kInline = 8;   // n > 8 is vanishingly rare in the API
    VARIANT  m_inline[kInline];
    VARIANT* m_p;
    size_t   m_n;
  };
