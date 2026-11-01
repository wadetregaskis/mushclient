#!/usr/bin/env python3
"""Guard: every control in resources/en.rc has a name a screen reader can say.

Windows gives an unnamed edit, combo box or list the text of the static control just
before it in the dialog (which is its Z order, and also where Alt+mnemonic on a label
sends focus).  A control with some other control in between is announced as just "edit".
A button's name is its caption, and that includes an owner-drawn colour swatch, which
never shows its caption, so a caption of "1" is what gets read out.

So:
  1. An EDITTEXT, COMBOBOX, LISTBOX, or list/tree control must directly follow a label:
     an LTEXT, RTEXT, CTEXT or a static CONTROL, with text.  A control whose name is set
     in code is listed in NAMED_IN_CODE.
  2. An owner-drawn button's caption must have letters in it.

Usage: check_dialog_labels.py [--selftest] [--list] [path to en.rc]
Exit status 0 = fine, 1 = problems (each printed).
"""

import os
import re
import sys

# (dialog, control): the label comes from somewhere else, and why.
NAMED_IN_CODE = {
}

# Dialogs not worth having a label for: they are one big box and the title says it.
LABEL_NOT_NEEDED = {
    ("IDD_EDIT", "IDC_EDIT_TEXT"),
}

CONTROL_LINE = re.compile(
    r'^\s*(LTEXT|RTEXT|CTEXT|GROUPBOX|PUSHBUTTON|DEFPUSHBUTTON|EDITTEXT|COMBOBOX|LISTBOX|CONTROL|ICON|SCROLLBAR)\b(.*)$')
DIALOG_LINE = re.compile(r'^\s*(\w+)\s+DIALOG(?:EX)?\b')
STRING = r'"(?:[^"]|"")*"'


def controls(lines):
    """Yields (dialog, kind, text, id, whole statement) in order."""
    dialog = None
    i = 0
    in_body = False
    while i < len(lines):
        line = lines[i]
        m = DIALOG_LINE.match(line)
        if m:
            dialog = m.group(1)
            in_body = False
            i += 1
            continue
        if dialog and re.match(r'^\s*BEGIN\b', line):
            in_body = True
            i += 1
            continue
        if dialog and re.match(r'^\s*END\b', line):
            dialog = None
            in_body = False
            i += 1
            continue
        if in_body:
            m = CONTROL_LINE.match(line)
            if m:
                statement = line
                while statement.rstrip().endswith((",", "|")) and i + 1 < len(lines):
                    i += 1
                    statement += " " + lines[i].strip()
                yield dialog, m.group(1), statement
        i += 1


def parse(statement, kind):
    text = ""
    rest = statement.split(kind, 1)[1].strip()
    m = re.match(STRING, rest)
    if m:
        text = m.group(0)[1:-1]
        rest = rest[m.end():].lstrip(", ")
    ident = rest.split(",", 1)[0].strip()
    return text, ident


def class_of(statement):
    m = re.search(r'"(\w+)"\s*,', statement.split(",", 2)[-1] if statement.count(",") > 2 else "")
    return m.group(1).upper() if m else ""


def check(text):
    problems = []
    previous_is_label = False
    previous_dialog = None
    for dialog, kind, statement in controls(text.splitlines()):
        if dialog != previous_dialog:
            previous_is_label = False
            previous_dialog = dialog
        label, ident = parse(statement, kind)
        klass = ""
        if kind == "CONTROL":
            found = re.findall(r'"(\w+)"', statement)
            klass = found[-1].upper() if found else ""

        needs_label = kind in ("EDITTEXT", "COMBOBOX", "LISTBOX") or \
            klass in ("SYSLISTVIEW32", "SYSTREEVIEW32", "RICHEDIT", "RICHEDIT20A", "RICHEDIT50W")

        if needs_label and not previous_is_label:
            if (dialog, ident) not in NAMED_IN_CODE and (dialog, ident) not in LABEL_NOT_NEEDED:
                problems.append("%s %s: no label directly before it" % (dialog, ident))

        if kind == "CONTROL" and klass == "BUTTON" and "BS_OWNERDRAW" in statement:
            if not re.search(r'[A-Za-z]', label):
                problems.append('%s %s: owner-drawn button captioned "%s"' % (dialog, ident, label))

        previous_is_label = kind in ("LTEXT", "RTEXT", "CTEXT") and bool(label.strip()) or \
            (kind == "CONTROL" and klass == "STATIC" and bool(label.strip()))
    return problems


def selftest():
    bad = '''IDD_X DIALOG 0,0,1,1
BEGIN
    LTEXT "Name:",IDC_STATIC,0,0,1,1
    PUSHBUTTON "Go",IDC_GO,0,0,1,1
    EDITTEXT IDC_A,0,0,1,1
    CONTROL "1",IDC_S,"Button",BS_OWNERDRAW |
                    WS_TABSTOP,0,0,1,1
END
'''
    good = '''IDD_X DIALOG 0,0,1,1
BEGIN
    LTEXT "Name:",IDC_STATIC,0,0,1,1
    EDITTEXT IDC_A,0,0,1,1
    CONTROL "Text colour",IDC_S,"Button",BS_OWNERDRAW |
                    WS_TABSTOP,0,0,1,1
END
'''
    if len(check(bad)) != 2:
        print("SELFTEST FAIL: expected two problems, got", check(bad))
        return 1
    if check(good):
        print("SELFTEST FAIL: expected none, got", check(good))
        return 1
    print("selftest ok")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    args = [a for a in argv[1:] if not a.startswith("--")]
    path = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "resources", "en.rc")
    with open(path, encoding="latin-1") as f:
        problems = check(f.read())
    for p in problems:
        print(p)
    print("%d problems" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
