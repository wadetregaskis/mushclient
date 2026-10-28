#!/usr/bin/env python3
# port_resources.py — the macOS/Linux port's resources, from a compiled en.rc.
#
# resources/en.rc is the one source of MUSHclient's dialogs, menus, bitmaps and
# icons.  Windows compiles it into locale/en.dll (resources/en.vcxproj) and MFC
# loads from that at run time.  The wx port has no DLL-resource loader, so its
# build (resources/CMakeLists.txt) compiles the same en.rc with llvm-rc and
# hands the resulting .res to this tool, which writes, into <outdir>:
#
#   mushclient_dialogs.rc   every dialog template (DIALOGEX), its combo-box
#                           presets (DLGINIT) and every menu, as the simple .rc
#                           text the wx dialog bridge parses
#                           (src/platform/wx_dialog_resource.cpp), numeric ids
#                           mapped back to resource.h symbols
#   art/bitmap/<id>.bmp     every BITMAP, as a .bmp file
#   art/icon/<id>.ico       every ICON, as a .ico file
#
# Any .res will do, rc.exe's included: tests/check_port_resources.py converts
# both compilers' output on Windows and requires them to come out the same.
#
#   python3 tools/port_resources.py <en.res> <resource.h> <outdir>

import os, struct, re, sys

def read_res(path):
    """{type: {name: data}} from a .res file: a sequence of RESOURCEHEADERs,
    each followed by its data, every entry DWORD-aligned."""
    b = open(path, "rb").read()
    def name(o):                    # ordinal (0xFFFF prefix), or UTF-16 string
        if struct.unpack_from("<H", b, o)[0] == 0xFFFF:
            return struct.unpack_from("<H", b, o + 2)[0], o + 4
        s = []
        while True:
            c = struct.unpack_from("<H", b, o)[0]; o += 2
            if not c:
                break
            s.append(chr(c))
        return "".join(s), o
    allres = {}
    o = 0
    while o < len(b):
        size, hsize = struct.unpack_from("<II", b, o)
        t, p = name(o + 8)
        n, p = name(p)
        if size:                    # the leading empty entry marks a 32-bit .res
            allres.setdefault(t, {})[n] = b[o + hsize:o + hsize + size]
        o = (o + hsize + size + 3) & ~3
    return allres

def bitmap_file(dib):
    """An RT_BITMAP resource (a DIB without its file header) as a .bmp file."""
    hsize, _w, _h, _planes, bits, comp = struct.unpack_from("<IiiHHI", dib, 0)
    used = struct.unpack_from("<I", dib, 32)[0] if hsize >= 36 else 0
    colours = used or (1 << bits if bits <= 8 else 0)
    masks = 12 if hsize == 40 and comp == 3 else 0      # BI_BITFIELDS
    off = 14 + hsize + masks + 4 * colours
    return struct.pack("<2sIHHI", b"BM", 14 + len(dib), 0, 0, off) + dib

def icon_file(group, icons):
    """An RT_GROUP_ICON resource and the RT_ICON images it names as a .ico
    file: the same directory, with each entry's resource id replaced by the
    offset of its image, and the images after it in order."""
    _res, typ, count = struct.unpack_from("<HHH", group, 0)
    images = [struct.unpack_from("<12sH", group, 6 + 14 * i) for i in range(count)]
    out = [struct.pack("<HHH", 0, typ, count)]
    off = 6 + 16 * count
    for entry, rid in images:
        out.append(entry + struct.pack("<I", off))
        off += len(icons[rid])
    out += [icons[rid] for _entry, rid in images]
    return b"".join(out)

def main(res_path, resh, outdir):
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
    # Menus likewise: IDR_/IDM_ (e.g. id 319 is IDR_PANETYPE, a menu, as well
    # as IDD_GLOBAL_PREFSP13, a dialog).
    menu2sym = {}
    for name, val in defines:
        if name.startswith(("IDR_", "IDM_")):
            menu2sym.setdefault(val, name)
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

    allres = read_res(res_path)
    res = lambda tid: allres.get(tid, {})

    dlgs, menus, dlginits = res(5), res(4), res(240)
    L = [
        "// MUSHclient's dialog and menu resources, for the macOS/Linux port.",
        "// GENERATED at build time from resources/en.rc (compiled by llvm-rc) by",
        "// tools/port_resources.py: edit en.rc, not this.  Numeric ids are mapped",
        "// back to resource.h symbols.  Parsed by the wx dialog bridge so that the",
        "// canonical dialogs/**/*.cpp render natively.",
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
        L.append(f"{menu2sym.get(rid) or sym(rid)} MENU")
        L.append("BEGIN"); L.extend(parse_menu(menus[rid])); L.append("END"); L.append("")

    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "mushclient_dialogs.rc"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))

    # Named by number: an id is often a bitmap AND an icon (IDR_MAINFRAME is
    # the toolbar strip and the app icon), so the type is the directory.
    for sub in ("bitmap", "icon"):
        os.makedirs(os.path.join(outdir, "art", sub), exist_ok=True)
    bitmaps, groups, icons = res(2), res(14), res(3)
    for rid, dib in bitmaps.items():
        open(os.path.join(outdir, "art", "bitmap", f"{rid}.bmp"), "wb").write(bitmap_file(dib))
    for rid, group in groups.items():
        open(os.path.join(outdir, "art", "icon", f"{rid}.ico"), "wb").write(icon_file(group, icons))

    print(f"wrote {outdir}: {len(dlgs)} dialogs, {len(menus)} menus, "
          f"{len(dlginits)} dlginit, {len(bitmaps)} bitmaps, {len(groups)} icons")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: port_resources.py <en.res> <resource.h> <outdir>")
    main(sys.argv[1], sys.argv[2], sys.argv[3])
