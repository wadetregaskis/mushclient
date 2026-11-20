////////////////////////////////////////////////////////////////
// CWindowPlacement 1996 Microsoft Systems Journal.
// If this code works, it was written by Paul DiLascia.
// If not, I don't know who wrote it.

////////////////
// CWindowPlacement reads and writes WINDOWPLACEMENT 
// from/to application profile and CArchive.
//
struct CWindowPlacement : public WINDOWPLACEMENT {
   CWindowPlacement();
   ~CWindowPlacement();
   
   // Read/write to app profile
   void GetProfileWP(LPCSTR lpKeyName);
   void WriteProfileWP(LPCSTR lpKeyName);

   // Save/restore window pos (from app profile)
   void Save(LPCSTR lpKeyName, CWnd* pWnd);
   BOOL Restore(LPCSTR lpKeyName, 
                CWnd* pWnd,
                const bool bDesktop);

   // The dpi rcNormalPosition was saved at; 0 for a placement saved in 96-dpi pixels
   // (by 5.x, or a 6.00 before it was kept).  Not part of WINDOWPLACEMENT, so
   // SetWindowPlacement (this) never sees it.
   int m_iSavedDpi;

#ifdef _WIN32
   // How Restore scales a saved rectangle for where the window is going (winplace.cpp).
   static void ScaleSaved (RECT & rc, const int iSavedDpi, const int iNowDpi,
                           const int iSystemDpi, const bool bInsideFrame);
#endif

   // Save/restore from archive
   friend CArchive& operator<<(CArchive& ar, const CWindowPlacement& wp);
   friend CArchive& operator>>(CArchive& ar, CWindowPlacement& wp);
};
