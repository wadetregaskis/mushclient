// platform/mfc_object.h
// CObject and related MFC base-class infrastructure stubs.
//
// MFC's CObject provides runtime type info and dynamic creation.
// On macOS we provide an empty base class so that all the classes that
// inherit from CObject still compile unchanged.

#pragma once

#ifndef _WIN32

// ── CRuntimeClass ─────────────────────────────────────────────────────────
// Each class's static CRuntimeClass records its name and a pointer to its
// base class's CRuntimeClass, forming a chain CObject ← … ← cls.  IsKindOf()
// and RUNTIME_CLASS() walk this chain — matching MFC, so the canonical code's
// `x->IsKindOf(RUNTIME_CLASS(T))` branching (used widely in doc.cpp, the views,
// the entity editors, …) takes the correct path on the port too.
struct CRuntimeClass {
    const char*    m_lpszClassName;
    CRuntimeClass* m_pBaseClass;
    // Is `this` the same as, or derived from, `pBase`?  Walk up the base chain.
    bool IsDerivedFrom(const CRuntimeClass* pBase) const {
        for (const CRuntimeClass* p = this; p; p = p->m_pBaseClass)
            if (p == pBase) return true;
        return false;
    }
};

// DECLARE_DYNAMIC / IMPLEMENT_DYNAMIC
// On Windows these emit a static CRuntimeClass member + GetRuntimeClass().
// We match that structure here so that RUNTIME_CLASS(X) works for classes
// that use DECLARE_DYNAMIC just as well as those using DECLARE_DYNCREATE.
// IMPLEMENT_* links the static CRuntimeClass to its base class's, so IsKindOf
// can walk the inheritance chain.
#define DECLARE_DYNAMIC(cls)                                           \
    public:                                                            \
    static CRuntimeClass class##cls;                                   \
    virtual CRuntimeClass* GetRuntimeClass() const                     \
        { return &cls::class##cls; }
#define IMPLEMENT_DYNAMIC(cls, base)                                   \
    CRuntimeClass cls::class##cls = { #cls, &base::class##base };

// DECLARE_SERIAL / IMPLEMENT_SERIAL — same treatment
#define DECLARE_SERIAL(classname)
#define IMPLEMENT_SERIAL(classname, baseclass, schema)

// CObject — minimal base class; needs IsKindOf() and classCObject for
// RUNTIME_CLASS(CObject) to work in ASSERT() calls throughout the code.
class CObject
{
public:
    static CRuntimeClass classCObject;
    virtual CRuntimeClass* GetRuntimeClass() const { return &CObject::classCObject; }
    // BOOL (int).  True when this object's runtime class is pClass or derives
    // from it — walks the m_pBaseClass chain emitted by IMPLEMENT_DYNAMIC.
    virtual int  IsKindOf(const CRuntimeClass* pClass) const {
        const CRuntimeClass* rc = GetRuntimeClass();
        return (rc && pClass && rc->IsDerivedFrom(pClass)) ? 1 : 0;
    }
    virtual ~CObject() {}
};

// Dynamic-creation macros
// Note: we force 'public:' so RUNTIME_CLASS() can access the static member
// even when DECLARE_DYNCREATE appears in a protected: section of a class.
#define DECLARE_DYNCREATE(cls)                                         \
    public:                                                            \
    static CRuntimeClass class##cls;                                   \
    virtual CRuntimeClass* GetRuntimeClass() const                     \
        { return &cls::class##cls; }

#define IMPLEMENT_DYNCREATE(cls, base)                                 \
    CRuntimeClass cls::class##cls = { #cls, &base::class##base };

// RUNTIME_CLASS returns a pointer to the class's static CRuntimeClass.
#define RUNTIME_CLASS(cls)  (&cls::class##cls)

// SHIM_RUNTIME_CLASS — for the header-only MFC shim base classes (CWnd, CDialog,
// CView, …) that don't use DECLARE/IMPLEMENT_DYNAMIC in a .cpp.  Emits the
// static CRuntimeClass (defined inline to avoid ODR issues — the shim headers
// are included by many TUs) linked to `base`, plus the GetRuntimeClass override.
// This completes the IsKindOf chain so canonical IMPLEMENT_DYNAMIC(cls, ShimBase)
// can reference &ShimBase::classShimBase, and runtime branching works.
#define SHIM_RUNTIME_CLASS(cls, base)                                  \
    public:                                                            \
    static inline CRuntimeClass class##cls = { #cls, &base::class##base }; \
    virtual CRuntimeClass* GetRuntimeClass() const override            \
        { return &cls::class##cls; }

// SHIM_RUNTIME_CLASS_ROOT — for shim classes that are their own chain root
// (CException, CAsyncSocket) and so don't derive from CObject.  No `override`
// (no virtual GetRuntimeClass in a base), and the base-class link is null.
#define SHIM_RUNTIME_CLASS_ROOT(cls)                                   \
    public:                                                            \
    static inline CRuntimeClass class##cls = { #cls, nullptr };        \
    virtual CRuntimeClass* GetRuntimeClass() const                     \
        { return &cls::class##cls; }

// CObject's static member — defined inline here; the header is included
// by many translation units so we use inline to avoid ODR violations.
// CObject is the chain root: no base class.
inline CRuntimeClass CObject::classCObject = { "CObject", nullptr };

#endif // !_WIN32
