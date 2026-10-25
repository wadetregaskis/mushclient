#!/usr/bin/env python3
# decompile_dialogs.py — recover MUSHclient's dialog + menu resources as .rc text.
#
# Why this exists: MUSHclient's MFC dialog LAYOUTS (the DIALOGEX/MENU templates)
# are not in the public source.  MUSHclient.rc in the repo (and upstream on
# GitHub) is a 144-line skeleton — icons/cursor/version only.  On Windows the
# dialogs are loaded at runtime from the MFC localisation satellite DLL
# (locale/en.dll), where the compiled RT_DIALOG / RT_MENU resources live.  The
# macOS/wx port has no DLL-resource loader, so to render the canonical
# dialogs/**/*.cpp natively (instead of hand-rebuilding each in wx) the wx MFC
# shim needs those layouts.  This tool decompiles them from en.dll back to .rc
# text, mapping numeric IDs to resource.h symbols.
#
# Source of en.dll: unpack the official installer (GPL software):
#   curl -O http://www.gammon.com.au/files/mushclient/mushclient506.exe
#   7z x mushclient506.exe -o/tmp/mc           # NSIS installer
#   ->  /tmp/mc/locale/en.dll
#
# Usage:
#   pip install pefile
#   python3 tools/decompile_dialogs.py /tmp/mc/locale/en.dll resource.h \
#           src/platform/mushclient_dialogs.rc
#
# Output is NOT compiled by the Windows resource compiler (Windows uses en.dll);
# it is bundled into the .app and parsed by the wx dialog loader.

import pefile, struct, re, sys

def main(dll, resh, out):
    # resource.h assigns many numeric ids to more than one symbol across
    # unrelated Win32 resource-type namespaces (dialog/menu/string/command), so
    # a bare "first definition wins" can label a real DIALOGEX block with a
    # command or menu symbol that happens to share its value (e.g. id 301 is
    # both ID_GETTING_STARTED, a command, and IDD_REGEXP_PROBLEM, the dialog
    # that actually lives there).  Two passes: IDD_-prefixed symbols (dialog
    # template ids, by MFC convention) win any collision; everything else
    # fills in around them.
    defines = []
    for line in open(resh, encoding="latin-1"):
        m = re.match(r'\s*#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)', line)
        if m:
            name, val = m.group(1), int(m.group(2), 0)
            if name.startswith("_APS_"):
                continue            # apstudio bookkeeping symbols, not real IDs
            defines.append((name, val))
    id2sym = {}
    for name, val in defines:
        if name.startswith("IDD_"):
            id2sym.setdefault(val, name)
    for name, val in defines:
        id2sym.setdefault(val, name)
    STD = {1: "IDOK", 2: "IDCANCEL", 3: "IDABORT", 4: "IDRETRY", 5: "IDIGNORE",
           6: "IDYES", 7: "IDNO", 9: "IDHELP", 0xFFFF: "IDC_STATIC"}

    def sym(i):
        if not isinstance(i, int):
            return str(i)
        return STD.get(i) or id2sym.get(i, str(i))

    def esc(s):
        if not isinstance(s, str):
            return '""'
        s = (s.replace('\\', '\\\\').replace('"', '""')
              .replace(chr(13), '\\r').replace(chr(10), '\\n').replace(chr(9), '\\t'))
        return '"' + s + '"'

    def rd(b, o):                   # sz_Or_Ord: ordinal (0xFFFF prefix), or UTF-16 string
        f = struct.unpack_from("<H", b, o)[0]
        if f == 0:
            return ("", o + 2)
        if f == 0xFFFF:
            return (struct.unpack_from("<H", b, o + 2)[0], o + 4)
        c = []
        while True:
            ch = struct.unpack_from("<H", b, o)[0]; o += 2
            if ch == 0:
                break
            c.append(ch)
        return ("".join(map(chr, c)), o)

    al = lambda o: (o + 3) & ~3
    CLS = {0x80: "Button", 0x81: "Edit", 0x82: "Static",
           0x83: "ListBox", 0x84: "ScrollBar", 0x85: "ComboBox"}

    def parse_dlg(b):               # handles both classic DLGTEMPLATE and DLGTEMPLATEEX
        dv, sig = struct.unpack_from("<HH", b, 0)
        ex = (sig == 0xFFFF and dv == 1)
        o = 0
        if ex:
            o += 4
            _hid, exs, st = struct.unpack_from("<III", b, o); o += 12
            ci, x, y, cx, cy = struct.unpack_from("<Hhhhh", b, o); o += 10
        else:
            st, exs = struct.unpack_from("<II", b, o); o += 8
            ci = struct.unpack_from("<H", b, o)[0]; o += 2
            x, y, cx, cy = struct.unpack_from("<hhhh", b, o); o += 8
        _menu, o = rd(b, o); _cls, o = rd(b, o); title, o = rd(b, o)
        font = None
        if st & 0x40:               # DS_SETFONT
            if ex:
                pt, _wt = struct.unpack_from("<HH", b, o); o += 4
                o += 2              # italic + charset
                face, o = rd(b, o); font = (pt, face)
            else:
                pt = struct.unpack_from("<H", b, o)[0]; o += 2
                face, o = rd(b, o); font = (pt, face)
        items = []
        for _ in range(ci):
            o = al(o)
            if ex:
                _ih, iex, ist = struct.unpack_from("<III", b, o); o += 12
                ix, iy, icx, icy = struct.unpack_from("<hhhh", b, o); o += 8
                iid = struct.unpack_from("<I", b, o)[0]; o += 4
            else:
                ist, iex = struct.unpack_from("<II", b, o); o += 8
                ix, iy, icx, icy = struct.unpack_from("<hhhh", b, o); o += 8
                iid = struct.unpack_from("<H", b, o)[0]; o += 2
            ic, o = rd(b, o); itt, o = rd(b, o)
            extra = struct.unpack_from("<H", b, o)[0]; o += 2 + extra
            items.append((iid, ic, itt, ist, iex, ix, iy, icx, icy))
        return dict(st=st, exs=exs, rect=(x, y, cx, cy), title=title, font=font, items=items)

    def parse_menu(b):              # classic MENUHEADER + MENUITEMs
        o = 4                       # WORD version + WORD cbHeaderSize
        out = []
        def items(o, depth):
            while True:
                flags = struct.unpack_from("<H", b, o)[0]; o += 2
                if flags & 0x10:    # MF_POPUP
                    txt, o = rd(b, o)
                    out.append("  " * depth + f'POPUP {esc(txt)}')
                    out.append("  " * depth + "BEGIN")
                    o = items(o, depth + 1)
                    out.append("  " * depth + "END")
                else:
                    mid = struct.unpack_from("<H", b, o)[0]; o += 2
                    txt, o = rd(b, o)
                    if txt == "" and mid == 0:
                        out.append("  " * depth + "MENUITEM SEPARATOR")
                    else:
                        out.append("  " * depth + f'MENUITEM {esc(txt)}, {sym(mid)}')
                if flags & 0x80:    # MF_END
                    return o
        items(o, 1)
        return out

    def parse_dlginit(b):           # RT_DLGINIT: {WORD id, WORD msg, DWORD len, data}* then WORD 0
        recs = []; o = 0
        while o + 2 <= len(b):
            idCtrl = struct.unpack_from("<H", b, o)[0]; o += 2
            if idCtrl == 0:
                break
            if o + 6 > len(b):
                break
            msg = struct.unpack_from("<H", b, o)[0]; o += 2
            ln  = struct.unpack_from("<I", b, o)[0]; o += 4
            data = b[o:o + ln]; o += ln
            recs.append((idCtrl, msg, data.split(b"\x00")[0].decode("latin-1", "replace")))
        return recs

    pe = pefile.PE(dll)

    def res(tid):
        d = {}
        for e in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            if e.id != tid:
                continue
            for de in e.directory.entries:
                ds = de.directory.entries[0].data.struct
                d[de.id] = pe.get_data(ds.OffsetToData, ds.Size)
        return d

    dlgs, menus, dlginits = res(5), res(4), res(240)
    L = [
        "// MUSHclient dialog + menu resources, decompiled from the MFC localisation",
        "// DLL (MUSHclient 5.06 locale/en.dll, RT_DIALOG + RT_MENU) by",
        "// tools/decompile_dialogs.py.  Numeric IDs mapped to resource.h symbols.",
        "//",
        "// NOT part of the Windows resource compile (Windows loads these from the",
        "// localisation DLL at runtime).  This file is bundled into the macOS .app",
        "// and parsed by the wx MFC-dialog shim so the canonical dialogs/**/*.cpp",
        "// render natively.  Regenerate with tools/decompile_dialogs.py.",
        '#include "resource.h"',
        '#include "afxres.h"',
        "",
    ]
    for rid in sorted(dlgs):
        d = parse_dlg(dlgs[rid])
        x, y, cx, cy = d['rect']
        L.append(f"{sym(rid)} DIALOGEX {x}, {y}, {cx}, {cy}")
        L.append(f"STYLE 0x{d['st']:08X}")
        if d['exs']:
            L.append(f"EXSTYLE 0x{d['exs']:08X}")
        if d['title']:
            L.append(f"CAPTION {esc(d['title'])}")
        if d['font']:
            L.append(f"FONT {d['font'][0]}, {esc(d['font'][1])}")
        L.append("BEGIN")
        for (iid, ic, itt, ist, iex, ix, iy, icx, icy) in d['items']:
            cl = CLS.get(ic, ic) if isinstance(ic, int) else ic
            tail = f', 0x{iex:08X}' if iex else ''
            L.append(f'    CONTROL {esc(itt)}, {sym(iid)}, {esc(cl)}, '
                     f'0x{ist:08X}, {ix}, {iy}, {icx}, {icy}{tail}')
        L.append("END")
        L.append("")
        if rid in dlginits:                 # combo-box preset items (CB_ADDSTRING)
            combo = [(cid, s) for (cid, msg, s) in parse_dlginit(dlginits[rid])
                     if msg == 0x0403]
            if combo:
                L.append(f"{sym(rid)} DLGINIT")
                L.append("BEGIN")
                for (cid, s) in combo:
                    L.append(f"    {sym(cid)}, {esc(s)}")
                L.append("END")
                L.append("")
    for rid in sorted(menus):
        L.append(f"{sym(rid)} MENU")
        L.append("BEGIN"); L.extend(parse_menu(menus[rid])); L.append("END"); L.append("")

    open(out, "w", encoding="latin-1").write("\n".join(L))
    print(f"wrote {out}: {len(dlgs)} dialogs, {len(menus)} menus, "
          f"{len(dlginits)} dlginit, {len(L)} lines")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: decompile_dialogs.py <en.dll> <resource.h> <out.rc>")
    main(sys.argv[1], sys.argv[2], sys.argv[3])
