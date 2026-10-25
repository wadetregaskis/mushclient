// pal/image_placement.h — where a world's background/foreground image goes.
//
// world.SetBackgroundImage / SetForegroundImage take a MODE (0-13) that decides
// whether the image is stretched, aligned to an edge or a corner, centred, or
// tiled.  This is the arithmetic of that decision, lifted out of the drawing
// code so it can be checked.
//
// WHY IT WAS LIFTED
//
// It lived inside a lambda whose only caller is COutputPane::OnPaint, and no
// in-app selftest reaches the paint handler at all -- every one of them exits
// from wxApp::OnInit, before the event loop AppKit issues drawRect: from
// (docs/AUDIT-2026-08.md).  So thirteen placement modes' worth of integer
// arithmetic -- centring, right/bottom alignment, two aspect-ratio scalings --
// had never executed under test, and a wrong one puts the user's wallpaper
// somewhere they did not ask for.
//
// The modes are Blit_Bitmap's !bUseRect placement (mushview.cpp), and the
// numbering is world-file/script API, so it is fixed:
//
//    0  stretch to the output window          7  right, centred vertically
//    1  stretch, keep aspect (to pane height) 8  bottom right
//    2  stretch to the OWNER (frame)          9  bottom, centred horizontally
//    3  stretch to owner, keep aspect        10  bottom left
//    4  top left (1:1)                       11  left, centred vertically
//    5  top, centred horizontally            12  centred both ways
//    6  top right                            13  tile
//
// Header-only and free of wx/MFC types so both the GUI adapter and the tests
// can use it.

#pragma once

namespace pal {

struct ImagePlacement {
    int  x = 0, y = 0;          // destination top-left
    int  w = 0, h = 0;          // destination size
    bool stretch = false;       // scale the source into w x h, else blit 1:1
    bool tile    = false;       // repeat across the client area from (0,0)
    // Tile counts, meaningful only when tile is true.  Deliberately "+1" so a
    // client area that is not a whole multiple of the image still has its last
    // partial row and column covered.
    int  across = 0, down = 0;
};

// imgW/imgH: the source image.  clientW/clientH: the text area being drawn on.
// ownerW/ownerH: the containing frame, which modes 2 and 3 scale to instead.
//
// Callers must reject a non-positive image before calling; the guard here is a
// backstop that returns an empty placement rather than dividing by zero.
inline ImagePlacement imagePlacement(int mode,
                                     int imgW, int imgH,
                                     int clientW, int clientH,
                                     int ownerW, int ownerH)
{
    ImagePlacement p;
    if (imgW <= 0 || imgH <= 0)
        return p;

    const int w = imgW, h = imgH;
    p.w = w; p.h = h;

    switch (mode) {
    case 0:  p.stretch = true; p.w = clientW; p.h = clientH; break;
    // Keep the source's aspect ratio while filling the available HEIGHT, so
    // the width follows from it.  Same for mode 3 against the owner.
    case 1:  p.stretch = true; p.w = (int)(clientH * (double) w / h); p.h = clientH; break;
    case 2:  p.stretch = true; p.w = ownerW;  p.h = ownerH; break;
    case 3:  p.stretch = true; p.w = (int)(ownerH * (double) w / h);  p.h = ownerH; break;

    case 4:  p.x = 0;                 p.y = 0;                 break;
    case 5:  p.x = (clientW - w) / 2; p.y = 0;                 break;
    case 6:  p.x = clientW - w;       p.y = 0;                 break;
    case 7:  p.x = clientW - w;       p.y = (clientH - h) / 2; break;
    case 8:  p.x = clientW - w;       p.y = clientH - h;       break;
    case 9:  p.x = (clientW - w) / 2; p.y = clientH - h;       break;
    case 10: p.x = 0;                 p.y = clientH - h;       break;
    case 11: p.x = 0;                 p.y = (clientH - h) / 2; break;
    case 12: p.x = (clientW - w) / 2; p.y = (clientH - h) / 2; break;

    case 13:
        p.tile   = true;
        p.across = clientW / w + 1;
        p.down   = clientH / h + 1;
        break;

    // An unknown mode draws nothing rather than guessing: the option is
    // script-settable, so a typo must not land the image somewhere arbitrary.
    default:
        p.w = 0; p.h = 0;
        break;
    }
    return p;
}

} // namespace pal
