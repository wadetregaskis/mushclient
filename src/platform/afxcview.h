// platform/afxcview.h — MFC collection-view stub for non-Windows builds
#pragma once
#ifndef _WIN32
#include "compat.h"

class CListView : public CView {
    SHIM_RUNTIME_CLASS(CListView, CView)
public:
    CListCtrl& GetListCtrl() { static CListCtrl s; return s; }
    virtual void OnDestroy() {}
    virtual void OnInitialUpdate() {}
};

class CTreeView : public CView {
public:
    CTreeCtrl& GetTreeCtrl() { static CTreeCtrl s; return s; }
    virtual void OnDestroy() {}
    virtual void OnInitialUpdate() {}
};
#endif // !_WIN32
