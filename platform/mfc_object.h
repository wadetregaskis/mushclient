// platform/mfc_object.h
// CObject and related MFC base-class infrastructure stubs.
//
// MFC's CObject provides runtime type info and dynamic creation.
// On macOS we provide an empty base class so that all the classes that
// inherit from CObject still compile unchanged.

#pragma once

#ifndef _WIN32

// DECLARE_DYNAMIC / IMPLEMENT_DYNAMIC
// On Windows these emit a static CRuntimeClass member + GetRuntimeClass().
// We match that structure here so that RUNTIME_CLASS(X) works for classes
// that use DECLARE_DYNAMIC just as well as those using DECLARE_DYNCREATE.
#define DECLARE_DYNAMIC(cls)                                           \
    public:                                                            \
    static CRuntimeClass class##cls;                                   \
    virtual CRuntimeClass* GetRuntimeClass() const                     \
        { return &cls::class##cls; }
#define IMPLEMENT_DYNAMIC(cls, base)                                   \
    CRuntimeClass cls::class##cls = { #cls };

// DECLARE_SERIAL / IMPLEMENT_SERIAL — same treatment
#define DECLARE_SERIAL(classname)
#define IMPLEMENT_SERIAL(classname, baseclass, schema)

// ── CRuntimeClass ─────────────────────────────────────────────────────────
// Provides just enough for RUNTIME_CLASS() and IsKindOf() to compile.
struct CRuntimeClass { const char* m_lpszClassName; };

// CObject — minimal base class; needs IsKindOf() and classCObject for
// RUNTIME_CLASS(CObject) to work in ASSERT() calls throughout the code.
class CObject
{
public:
    static CRuntimeClass classCObject;
    virtual CRuntimeClass* GetRuntimeClass() const { return &CObject::classCObject; }
    virtual int  IsKindOf(const CRuntimeClass*) const { return 0; }  // returns BOOL (int)
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
    CRuntimeClass cls::class##cls = { #cls };

// RUNTIME_CLASS returns a pointer to the class's static CRuntimeClass.
// Used as argument to IsKindOf(); our stub always returns false so the
// returned pointer just needs to be valid (non-null).
#define RUNTIME_CLASS(cls)  (&cls::class##cls)

// CObject's static member — defined inline here; the header is included
// by many translation units so we use inline to avoid ODR violations.
inline CRuntimeClass CObject::classCObject = { "CObject" };

#endif // !_WIN32
