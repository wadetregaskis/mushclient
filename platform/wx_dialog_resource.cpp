// platform/wx_dialog_resource.cpp — see wx_dialog_resource.h.
//
// A small, forgiving parser for the subset of .rc syntax our decompiler emits
// (tools/decompile_dialogs.py): DIALOGEX blocks of generic CONTROL statements,
// and MENU blocks of POPUP/MENUITEM. Not a general .rc parser.

#ifndef _WIN32

#include "wx_dialog_resource.h"
#include <cctype>
#include <cstdlib>

namespace {

inline std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// True if `line` begins with `kw` followed by a space/tab/end (a whole word).
inline bool startsWord(const std::string& line, const char* kw)
{
    size_t n = 0; while (kw[n]) ++n;
    if (line.compare(0, n, kw) != 0) return false;
    return line.size() == n || std::isspace((unsigned char)line[n]);
}

// First whitespace-delimited token of `line`.
inline std::string firstToken(const std::string& line)
{
    size_t i = 0; while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
    return line.substr(0, i);
}

} // namespace

// ── A cursor over one .rc statement, for field-by-field parsing ─────────────
// Understands quoted strings (RC "" escape + \r \n \t \\ escapes) and
// comma-separated fields.
struct RcCursor
{
    const std::string& s;
    size_t i = 0;
    explicit RcCursor(const std::string& str, size_t start = 0) : s(str), i(start) {}

    void skipWs() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    void skipComma() { skipWs(); if (i < s.size() && s[i] == ',') ++i; skipWs(); }
    bool atEnd() { skipWs(); return i >= s.size(); }

    // Parse a "..."-quoted string into `out`; returns false if not at a quote.
    bool parseString(std::string& out)
    {
        skipWs();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size())
        {
            char c = s[i];
            if (c == '"')
            {
                if (i + 1 < s.size() && s[i + 1] == '"') { out += '"'; i += 2; continue; }
                ++i; return true;             // closing quote
            }
            if (c == '\\' && i + 1 < s.size())
            {
                char n = s[i + 1];
                switch (n)
                {
                    case 'r': out += '\r'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    default: out += n; break;
                }
                i += 2; continue;
            }
            out += c; ++i;
        }
        return true;                          // unterminated; tolerate
    }

    // Read a bare token up to the next comma (trimmed).
    std::string parseToken()
    {
        skipWs();
        size_t start = i;
        while (i < s.size() && s[i] != ',') ++i;
        return trim(s.substr(start, i - start));
    }
};

void RcResources::LoadSymbols(const std::string& h)
{
    size_t pos = 0;
    while (pos < h.size())
    {
        size_t eol = h.find('\n', pos);
        std::string line = trim(h.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = (eol == std::string::npos) ? h.size() : eol + 1;

        // #define NAME value   (value: decimal or 0xHEX; ignore expressions)
        if (line.compare(0, 7, "#define") != 0) continue;
        RcCursor c(line, 7);
        c.skipWs();
        size_t ns = c.i;
        while (c.i < line.size() && (std::isalnum((unsigned char)line[c.i]) || line[c.i] == '_')) ++c.i;
        std::string name = line.substr(ns, c.i - ns);
        if (name.empty()) continue;
        c.skipWs();
        size_t vs = c.i;
        while (c.i < line.size() && !std::isspace((unsigned char)line[c.i])) ++c.i;
        std::string val = line.substr(vs, c.i - vs);
        if (val.empty()) continue;
        char* end = nullptr;
        long v = std::strtol(val.c_str(), &end, 0);   // base 0: handles 0x and decimal
        if (end && *end == '\0')
            m_symbols.emplace(name, (int)v);           // first definition wins
    }
}

int RcResources::ResolveSymbol(const std::string& name, bool* found) const
{
    if (found) *found = true;
    // Standard MFC/Windows IDs live in afxres.h/winuser.h, not resource.h, so
    // resolve them here (matching tools/decompile_dialogs.py's table).
    if (name == "IDC_STATIC") return -1;
    if (name == "IDOK")       return 1;
    if (name == "IDCANCEL")   return 2;
    if (name == "IDABORT")    return 3;
    if (name == "IDRETRY")    return 4;
    if (name == "IDIGNORE")   return 5;
    if (name == "IDYES")      return 6;
    if (name == "IDNO")       return 7;
    if (name == "IDCLOSE")    return 8;
    if (name == "IDHELP")     return 9;
    auto it = m_symbols.find(name);
    if (it != m_symbols.end()) return it->second;
    if (found) *found = false;
    return 0;
}

// Resolve a token that is a decimal/0xHEX literal or a symbol.
static long s_dummy;
long resolveValue(const RcResources& res, const std::string& tok, bool* ok)
{
    if (ok) *ok = true;
    std::string t = trim(tok);
    if (t.empty()) { if (ok) *ok = false; return 0; }
    if ((t[0] == '-') || std::isdigit((unsigned char)t[0]) ||
        (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')))
    {
        char* end = nullptr;
        long v = std::strtol(t.c_str(), &end, 0);
        if (end && *end == '\0') return v;
    }
    return res.ResolveSymbol(t, ok);
}

bool RcResources::ParseRc(const std::string& rc)
{
    enum { NONE, DIALOG, MENU, DLGINIT } mode = NONE;
    RcDialog curDlg;
    RcMenu   curMenu;
    std::vector<std::vector<RcMenuItem>*> menuStack;   // for nested POPUPs
    int blockDepth = 0;
    int dlginitDlgId = 0;                                          // dialog a DLGINIT block targets
    std::unordered_map<int, std::vector<std::string>> dlginitItems; // control id -> combo items

    size_t pos = 0;
    while (pos < rc.size())
    {
        size_t eol = rc.find('\n', pos);
        std::string raw = rc.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? rc.size() : eol + 1;
        std::string line = trim(raw);

        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 2, "//") == 0) continue;

        std::string tok0 = firstToken(line);

        // Dialog / menu headers: "<SYM> DIALOGEX ..." or "<SYM> MENU"
        size_t sp = line.find(' ');
        if (sp != std::string::npos)
        {
            std::string rest = trim(line.substr(sp + 1));
            if (startsWord(rest, "DIALOGEX") || startsWord(rest, "DIALOG"))
            {
                curDlg = RcDialog();
                curDlg.id = (int)resolveValue(*this, tok0, nullptr);   // symbol or raw number
                size_t dp = rest.find(' ');
                RcCursor c(rest, dp == std::string::npos ? rest.size() : dp + 1);
                bool ok;
                curDlg.x  = (int)resolveValue(*this, c.parseToken(), &ok); c.skipComma();
                curDlg.y  = (int)resolveValue(*this, c.parseToken(), &ok); c.skipComma();
                curDlg.cx = (int)resolveValue(*this, c.parseToken(), &ok); c.skipComma();
                curDlg.cy = (int)resolveValue(*this, c.parseToken(), &ok);
                mode = DIALOG;
                continue;
            }
            if (startsWord(rest, "MENUEX") || startsWord(rest, "MENU"))
            {
                curMenu = RcMenu();
                curMenu.id = (int)resolveValue(*this, tok0, nullptr);   // symbol or raw number
                menuStack.clear();
                mode = MENU;
                continue;
            }
            if (startsWord(rest, "DLGINIT"))
            {
                // Combo-box preset items for the dialog of the same id (emitted
                // right after that dialog's block, so it is already parsed).
                dlginitDlgId = (int)resolveValue(*this, tok0, nullptr);
                dlginitItems.clear();
                mode = DLGINIT;
                continue;
            }
        }

        if (mode == DIALOG)
        {
            if (startsWord(line, "STYLE"))   { curDlg.style   = (unsigned)resolveValue(*this, trim(line.substr(5)), nullptr); continue; }
            if (startsWord(line, "EXSTYLE")) { curDlg.exStyle = (unsigned)resolveValue(*this, trim(line.substr(7)), nullptr); continue; }
            if (startsWord(line, "CAPTION")) { RcCursor c(line, 7); c.parseString(curDlg.caption); continue; }
            if (startsWord(line, "FONT"))
            {
                RcCursor c(line, 4);
                curDlg.fontPt = (int)resolveValue(*this, c.parseToken(), nullptr); c.skipComma();
                c.parseString(curDlg.fontFace);
                continue;
            }
            if (startsWord(line, "BEGIN")) { ++blockDepth; continue; }
            if (startsWord(line, "END"))
            {
                if (--blockDepth <= 0) { m_dialogs[curDlg.id] = curDlg; mode = NONE; blockDepth = 0; }
                continue;
            }
            if (startsWord(line, "CONTROL"))
            {
                RcControl ctl;
                RcCursor c(line, 7);
                bool ok;
                c.parseString(ctl.text);                       c.skipComma();
                ctl.id        = (int)resolveValue(*this, c.parseToken(), &ok); if (!ok) ++m_unresolved; c.skipComma();
                c.parseString(ctl.className);                  c.skipComma();
                ctl.style     = (unsigned)resolveValue(*this, c.parseToken(), nullptr); c.skipComma();
                ctl.x         = (int)resolveValue(*this, c.parseToken(), nullptr); c.skipComma();
                ctl.y         = (int)resolveValue(*this, c.parseToken(), nullptr); c.skipComma();
                ctl.cx        = (int)resolveValue(*this, c.parseToken(), nullptr); c.skipComma();
                ctl.cy        = (int)resolveValue(*this, c.parseToken(), nullptr);
                if (!c.atEnd()) { c.skipComma(); ctl.exStyle = (unsigned)resolveValue(*this, c.parseToken(), nullptr); }
                curDlg.controls.push_back(ctl);
                continue;
            }
        }
        else if (mode == MENU)
        {
            if (startsWord(line, "BEGIN"))
            {
                menuStack.push_back(menuStack.empty() ? &curMenu.items
                                                      : &menuStack.back()->back().children);
                continue;
            }
            if (startsWord(line, "END"))
            {
                if (!menuStack.empty()) menuStack.pop_back();
                if (menuStack.empty()) { m_menus[curMenu.id] = curMenu; mode = NONE; }
                continue;
            }
            if (menuStack.empty()) continue;
            if (startsWord(line, "POPUP"))
            {
                RcMenuItem it; it.isPopup = true;
                RcCursor c(line, 5); c.parseString(it.text);
                menuStack.back()->push_back(it);
                continue;
            }
            if (startsWord(line, "MENUITEM"))
            {
                RcMenuItem it;
                RcCursor c(line, 8);
                std::string firstArg = c.parseToken();
                if (firstArg == "SEPARATOR") { it.isSeparator = true; }
                else
                {
                    RcCursor c2(line, 8); c2.parseString(it.text); c2.skipComma();
                    it.id = (int)resolveValue(*this, c2.parseToken(), nullptr);
                }
                menuStack.back()->push_back(it);
                continue;
            }
        }
        else if (mode == DLGINIT)
        {
            if (startsWord(line, "BEGIN")) continue;
            if (startsWord(line, "END"))
            {
                // Attach the accumulated combo items to the (already-parsed)
                // dialog's controls, matched by control id.
                auto dit = m_dialogs.find(dlginitDlgId);
                if (dit != m_dialogs.end())
                    for (RcControl& ctl : dit->second.controls)
                    {
                        auto mit = dlginitItems.find(ctl.id);
                        if (mit != dlginitItems.end()) ctl.comboItems = mit->second;
                    }
                mode = NONE;
                continue;
            }
            // Item line:  <control-id>, "string"
            RcCursor c(line);
            bool ok;
            int cid = (int)resolveValue(*this, c.parseToken(), &ok);
            c.skipComma();
            std::string item;
            if (ok && c.parseString(item)) dlginitItems[cid].push_back(item);
            continue;
        }
    }
    return true;
}

const RcDialog* RcResources::FindDialog(int id) const
{
    auto it = m_dialogs.find(id);
    return it == m_dialogs.end() ? nullptr : &it->second;
}
const RcMenu* RcResources::FindMenu(int id) const
{
    auto it = m_menus.find(id);
    return it == m_menus.end() ? nullptr : &it->second;
}
size_t RcResources::ControlCount() const
{
    size_t n = 0;
    for (auto& kv : m_dialogs) n += kv.second.controls.size();
    return n;
}

#ifdef WX_DIALOG_RESOURCE_TEST
// Standalone test: parse resource.h + the recovered .rc and print stats.
//   c++ -std=c++17 -DWX_DIALOG_RESOURCE_TEST platform/wx_dialog_resource.cpp -o /tmp/rctest
//   /tmp/rctest resource.h platform/mushclient_dialogs.rc
#include <cstdio>
#include <fstream>
#include <sstream>
static std::string slurp(const char* p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
int main(int argc, char** argv)
{
    if (argc != 3) { std::fprintf(stderr, "usage: rctest resource.h dialogs.rc\n"); return 2; }
    RcResources r;
    r.LoadSymbols(slurp(argv[1]));
    r.ParseRc(slurp(argv[2]));
    std::printf("dialogs=%zu menus=%zu controls=%zu unresolved-control-ids=%d\n",
                r.DialogCount(), r.MenuCount(), r.ControlCount(), r.UnresolvedSymbols());
    bool ok;
    int aboutId = r.ResolveSymbol("IDD_ABOUTBOX", &ok);
    const RcDialog* d = ok ? r.FindDialog(aboutId) : nullptr;
    if (d)
    {
        std::printf("\nIDD_ABOUTBOX (%d): \"%s\" %dx%d font=%dpt \"%s\" controls=%zu\n",
                    d->id, d->caption.c_str(), d->cx, d->cy, d->fontPt, d->fontFace.c_str(),
                    d->controls.size());
        for (size_t k = 0; k < d->controls.size() && k < 6; ++k)
        {
            const RcControl& c = d->controls[k];
            std::printf("  id=%d cls=%-9s style=0x%08X @(%d,%d,%d,%d) text=\"%s\"\n",
                        c.id, c.className.c_str(), c.style, c.x, c.y, c.cx, c.cy, c.text.c_str());
        }
    }
    // Spot-check a prefs page resolves + has controls.
    int p1 = r.ResolveSymbol("IDD_PREFS_P1", &ok);
    const RcDialog* pd = ok ? r.FindDialog(p1) : nullptr;
    std::printf("\nIDD_PREFS_P1: %s (%zu controls)\n",
                pd ? pd->caption.c_str() : "(not found)", pd ? pd->controls.size() : 0);
    return 0;
}
#endif

#endif // !_WIN32
