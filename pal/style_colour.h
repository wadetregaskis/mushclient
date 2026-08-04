// pal/style_colour.h — a style's colour indices → concrete RGB.
//
// This is the arithmetic of CMUSHclientDoc::GetStyleRGB (doc_shared.cpp),
// lifted out so it can be evaluated with a palette that is NOT the document's
// live members.  There is still exactly ONE body: GetStyleRGB calls this.
//
// WHY IT WAS LIFTED
//
// SOutputRun carries fg/bg already RESOLVED, baked at emit time.  A palette
// change therefore could not repaint the existing scrollback — the resolved
// values were all the view had — so the port re-emitted the entire buffer
// through RebuildOutputView: clear the pane, reset the emission cursor, and
// re-materialise every line, every run and every std::string, then rebuild the
// whole wrap cache.  At a 5,000-line buffer that is merely wasteful; with
// max_output_lines = 0 (unlimited) it is unbounded, and world.SetNormalColour
// is something a script can call.
//
// Carrying the style INPUTS alongside the resolved output lets the view
// re-resolve in place instead: same colours, no allocation, no re-emit, no
// wrap rebuild.
//
// The bit constants are duplicated here rather than pulled in from
// OtherTypes.h, because this header is included by wx TUs that must not pull
// in the MFC/Win32 shim (the same rule platform/output_types.h documents for
// action_type).  doc_shared.cpp static_asserts every one of them against the
// canonical definition, so the copy cannot drift silently.

#pragma once

namespace pal {

// Mirrors of OtherTypes.h — asserted equal in doc_shared.cpp.
enum : unsigned short {
    kStyleHilite     = 0x0001,   // HILITE     (bold)
    kStyleInverse    = 0x0008,   // INVERSE
    kStyleColourType = 0x0300,   // COLOURTYPE (mask)
    kColourAnsi      = 0x0000,   // COLOUR_ANSI
    kColourCustom    = 0x0100,   // COLOUR_CUSTOM
    kColourRgb       = 0x0200,   // COLOUR_RGB
};

// Everything a resolution depends on that is not in the style itself.
// m_bAlternativeInverse belongs here because it changes which half of an
// inverse pair gets the bold table — it is a resolution input, not a flag the
// renderer applies afterwards.
// NOTE the width: COLORREF is a 32-bit uint32_t (platform/windows_types.h:101)
// and a DWORD on Windows.  `unsigned long` is 64-bit on LP64, so declaring
// these as long and casting a COLORREF array through `(const unsigned long*)`
// reinterprets a 32-bit table as 64-bit and reads garbage -- the same LP64
// trap that once corrupted saved worlds through the option accessors.  A
// static_assert in doc_shared.cpp pins the width against COLORREF itself.
struct StylePalette {
    unsigned int  normal[8];        // m_normalcolour
    unsigned int  bold[8];          // m_boldcolour
    unsigned int  customText[16];   // m_customtext   (MAX_CUSTOM)
    unsigned int  customBack[16];   // m_customback
    bool          alternativeInverse = false;   // m_bAlternativeInverse
};

// styleFlags: the style's iFlags already masked with STYLE_BITS.
// foreIdx/backIdx: the style's iForeColour/iBackColour — a palette index for
// ANSI, a custom-colour index (in iForeColour only) for CUSTOM, and a literal
// COLORREF for RGB.
// xterm256: the 256-entry xterm table (utilities_shared.cpp).  Passed in rather
// than referenced so this header stays free of globals; indices >= 8 read it
// and are palette-INdependent, which is why an xterm-256 run needs no
// re-resolution when the user edits the eight ANSI pairs.
inline void resolveStyleColours(unsigned short styleFlags,
                                long foreIdx, long backIdx,
                                const StylePalette& pal,
                                const unsigned int* xterm256,
                                unsigned int& colour1, unsigned int& colour2)
{
    const unsigned short type = styleFlags & kStyleColourType;
    const bool inverse = (styleFlags & kStyleInverse) != 0;
    const bool hilite  = (styleFlags & kStyleHilite)  != 0;

    if (type == kColourCustom)
    {
        // The custom colour is stored in iForeColour ONLY; the pair comes from
        // the two custom tables at that one index.
        const int i = (foreIdx >= 0 && foreIdx < 16) ? (int) foreIdx : 0;
        if (inverse) { colour1 = pal.customBack[i]; colour2 = pal.customText[i]; }
        else         { colour1 = pal.customText[i]; colour2 = pal.customBack[i]; }
        return;
    }

    if (type == kColourRgb)
    {
        // Literal colours: nothing to resolve, and nothing a palette edit
        // may touch.  MXP <color> and trigger "other" colours land here.
        if (inverse) { colour1 = (unsigned int) backIdx; colour2 = (unsigned int) foreIdx; }
        else         { colour1 = (unsigned int) foreIdx; colour2 = (unsigned int) backIdx; }
        return;
    }

    // ── ANSI ────────────────────────────────────────────────────────────────
    const int fi = (foreIdx >= 0 && foreIdx < 256) ? (int) foreIdx : 0;
    const int bi = (backIdx >= 0 && backIdx < 256) ? (int) backIdx : 0;

    if (pal.alternativeInverse)
    {
        // "Alternative inverse": the indices keep their roles and only the
        // OUTPUTS swap, so the bold table still applies to the foreground
        // index — which is the whole point of the option.
        const unsigned int fore = (fi >= 8) ? xterm256[fi]
                                 : (hilite ? pal.bold[fi] : pal.normal[fi]);
        const unsigned int back = (bi >= 8) ? xterm256[bi] : pal.normal[bi];
        if (inverse) { colour1 = back; colour2 = fore; }
        else         { colour1 = fore; colour2 = back; }
        return;
    }

    // The old way: swap the INDICES first, so after an inverse the bold table
    // applies to what was the background index.
    const int iFore = inverse ? bi : fi;
    const int iBack = inverse ? fi : bi;

    colour1 = (iFore >= 8) ? xterm256[iFore]
            : (hilite ? pal.bold[iFore] : pal.normal[iFore]);
    colour2 = (iBack >= 8) ? xterm256[iBack] : pal.normal[iBack];
}

} // namespace pal
