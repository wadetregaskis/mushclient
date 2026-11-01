"""tricky.py -- the patterns most likely to tell PCRE 8.45 and PCRE2 apart, and subjects
that show it.  Used by make_corpus.py."""

ALL = [chr(c) for c in range(0x21, 0x7f)]


def patterns():
    out = []
    # every character after a backslash, alone, in a class, and quantified
    for c in ALL:
        e = "\\" + c
        out += [e, "a" + e + "b", "[" + e + "]", "[a" + e + "z]", e + "{2}", e + "*", "(" + e + ")", "^" + e + "$"]
        # and in class ranges
        out += ["[" + e + "-z]", "[a-" + e + "]", "[" + e + "-]", "[-" + e + "]"]
    # POSIX classes and shorthands next to a hyphen
    sets = [r"\d", r"\D", r"\w", r"\W", r"\s", r"\S", r"\h", r"\H", r"\v", r"\V", "[:alpha:]", "[:digit:]", "[:^digit:]",
            "[:word:]", r"\pL", r"\p{Lu}", r"\P{L}"]
    for s in sets:
        out += ["[" + s + "-_]", "[" + s + "-a]", "[a-" + s + "]", "[" + s + "-]", "[-" + s + "]", "[" + s + "-" + s + "]",
                "[x" + s + "-y]", "[^" + s + "-_]", "[" + s + r"\-_]", "[a-c" + s + "-z]", "[" + s + "-a-z]"]
    out += ["[a-z-9]", "[a-z-]", "[-a-z]", "[a-c-e-g]", "[+--]", "[--/]", "[---]", "[a-]", "[-]", "[]-a]", "[^]-a]", r"[\]-a]",
            r"[a-\]]", r"[\x41-\x5a]", r"[\x{41}-\x{5a}]", r"[\101-\132]", r"[\n-\r]", r"[\b]", r"[\b-c]", "[z-a]", r"[\e-a]",
            r"[\Qa-z\E]", r"[\Qa\E-z]", r"[a-\Qz\E]", r"[\w\Q-\E_]", "[[]", "[[a]", "[]a]", "[^]a]", "[]]", "[a]]", "[[:alpha:]]",
            "[[:foo:]]", "[:alpha:]", "[[:alpha:]", "[[.a.]]", "[[=a=]]", "[a[:digit:]b]", "[[:<:]]", "[[:>:]]"]
    # braces
    for q in ["{,3}", "{ 2 }", "{2, 3}", "{2 ,3}", "{ 2,3 }", "{,}", "{}", "{ }", "{a}", "{1,a}", "{a,1}", "{1,2,3}", "{ ,3}", "{, 3}",
              "{2,}", "{ 2,}", "{2, }", "{2}", "{2,3}", "{0}", "{0,}", "{0,0}", "{\t2}", "{2\t}", "{3,2}", "{65535}", "{65536}", "{1,65536}",
              "{2}?", "{2}+", "{,3}?", "{,3}+", "{ 2 }?", "{2,3}?", "{"]:
        out += ["x" + q, "^x" + q + "$", q, "(x)" + q, "[x]" + q, r"\d" + q, "(?:ab)" + q, ".*" + q, "x" + q + "y"]
    out += [r"\x{41}", r"\x{ 41 }", r"\x{}", r"\x", r"\xZ", r"\x4", r"\o{101}", r"\o{}", r"\p{L}", r"\p{ L }", r"\p{Foo}", r"\pZ", r"\p",
            r"\g{1}(a)", r"(a)\g{1}", r"(a)\g{-1}", r"(a)\g{ 1 }", r"(?<n>a)\k{n}", r"(?<n>a)\k{ n }", r"(?<n>a)\k<n>", r"(?<n>a)\k'n'",
            r"(?<n>a)\g{n}", r"(?P<n>a)(?P=n)", r"(?P<n>a)(?P>n)", r"(?<n>a)(?&n)", r"(a)(?1)", r"(a)(?-1)", r"(?+1)(a)", r"(a)\g<1>",
            r"(a)\g'1'", r"(a)\g1", r"\N{U+41}", r"\N{foo}", r"\N", r"\N+", r"\C", r"\R", r"\X", r"\K", r"a\Kb", r"\G", r"\A", r"\Z", r"\z",
            r"\Q", r"\Qa", r"\Qa\E", r"\E", r"a\Eb", r"\Q\E", r"\Q{\E", r"x\Q{,3}\E", r"\cA", r"\c", "\\c\xe9", r"\ca{2}", r"\e", r"\f", r"\a", r"\t", r"\0",
            r"\00", r"\012", r"\400", r"\777", r"\o{400}", r"\x{100}", r"\x{ff}", r"\xff", r"\x{d800}", r"\x{110000}", r"\u0041", r"\U00000041",
            r"\L", r"\l", r"\N{name}", r"\8", r"\9", r"\80", r"\89", r"\18", r"\81", r"a\8b", r"a\9b", r"(a)\8", r"(a)(b)(c)(d)(e)(f)(g)(h)\8",
            r"\8(a)(b)(c)(d)(e)(f)(g)(h)", r"(a)(b)(c)(d)(e)(f)(g)\8(h)", r"(a)(b)(c)(d)(e)(f)(g)(h)(i)\9", r"(a)\1", r"\1(a)", r"\1", r"\7", r"(a)\2", r"\10",
            r"\11", r"\100", r"[\8]", r"[\9]", r"[\1]", r"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\10", r"(a)(b)(c)(d)(e)(f)(g)(h)(i)\10"]
    # \K and lookaround
    out += [r"(?=a\K)", r"(?=a\Kb)ab", r"(?<=a\K)b", r"(?!a\K)b", r"(?<!a\K)b", r"foo(?=bar\K)", r"a(?=\K)b", r"(?=\K)"]
    # lookbehind
    out += [r"(?<=a{1,3})b", r"(?<=a|bc)d", r"(?<=a+)b", r"(?<=a*)b", r"(?<=\K)", r"(?<=(a))b", r"(?<=\bfoo)bar", r"(?<!a|bc)d",
            r"(?<=ab?)c", r"(?<=(?:a|bc)d)e", r"(?<=\R)b", r"(?<=\X)b", r"(?<=\C)b"]
    # group names
    n32, n33 = "a" * 32, "a" * 33
    out += ["(?<" + n32 + ">x)", "(?<" + n33 + ">x)", "(?P<" + n32 + ">x)", "(?P<" + n33 + ">x)", "(?<a>x)(?<a>y)", "(?J)(?<a>x)|(?<a>y)", "(?|(?<a>x)|(?<a>y))",
            "(?<1a>x)", "(?<a-b>x)", "(?<>x)", "(?<a", "(?'a'x)", "(?'a", "(?J)(?<n>a)|(?<n>b)\\k<n>", "(?<\xe9>x)"]
    # quantifiers on odd things
    out += ["^*", "^+", "^?", "^{2}", "$*", "$+", "$?", "$a", "a$b", r"\b*", r"\b+", r"\b?", r"\B*", r"\A*", r"\Z*", r"\z*", r"\G*", "(?=a)*", "(?=a)+", "(?=a)?",
            "(?=a){2}", "(?!a)*", "(?<=a)*", "(?<!a)+", "*", "+", "?", "a**", "a*+", "a++", "a?+", "a+*", "a?*", "a*?", "a+?", "a??", "a{2}{3}", "a{2}*", "(a*)*",
            "(a*)+", "(a|b)*c", "(?:)*", "()*", "()+", "(|a)*", "(a|)*", "[a]*", ".**", "(*ACCEPT)a", "a(*ACCEPT)", "a(*FAIL)", "(*F)", "a(*COMMIT)b", "a(*PRUNE)b",
            "a(*SKIP)b", "a(*THEN)b", "(*UTF8)a", "(*UTF)a", "(*UCP)a", "(*CR)a", "(*LF)a", "(*CRLF)a", "(*ANY)a", "(*ANYCRLF)a", "(*BSR_ANYCRLF)a", "(*BSR_UNICODE)a",
            "(*NO_START_OPT)a", "(*NO_AUTO_POSSESS)a", "(*NOTEMPTY)a", "(*NOTEMPTY_ATSTART)a", "(*NO_JIT)a", "(*LIMIT_MATCH=10)a", "(*LIMIT_RECURSION=10)a",
            "(*LIMIT_DEPTH=10)a", "(*LIMIT_HEAP=10)a", "(*FOO)a", "(*MARK:x)a", "(*:x)a"]
    # groups, conditions, options, comments, callouts
    out += ["(?i)a", "(?-i)a", "(?i:a)b", "(?i)a(?-i)b", "(?s).", "(?m)^a$", "(?x) a b # c", "(?x)a#[\nb", "(?x)a #{,3}\n{,3}", "(?x:a{ 2 })", "(?x)a{ 2 }", "(?xx)[a b]", "(?n)(a)", "(?^)a", "(?^i)a",
            "(?U)a+", "(?J)a", "(?X)a", "(?#comment)a", "(?#[)a", "(?#{,3})a", "(?#", "(?>a)", "(?|(a)|(b))\\1", "(?|(a)|(b))(c)", "(?(1)a|b)", "(a)?(?(1)b|c)", "(?(?=a)ab|c)", "(?(R)a|b)", "(?(R1)a|b)", "(?(DEFINE)(?<x>a))(?&x)",
            "(?(<n>)a|b)(?<n>c)", "(?('n')a|b)(?<n>c)", "(?(n)a|b)(?<n>c)", "(?R)", "a(?R)?b", "(?0)", "(?1)", "(a)(?1)", "(?C)a", "(?C1)a", "(?C255)a", "(?C256)a", "(?C'a')b", "(?C\"a\")b", "(?C{a})b", "(?", "(", ")", "(a", "a)", "[", "[a", "]", "a]", "}", "a}", "{a", "\\",
            "a\\", "(?P", "(?Pa)", "(?P<", "(?P=", "(?P>", "(?<", "(?<=", "(?<!", "(?=", "(?!", "(?:", "(?>", "(?|", "(?^", "(?a)", "(?aD)a", "(?aT)a", "(?aP)a", "(?r)a", "(?*a)b", "(*atomic:a)", "(*pla:a)", "(*positive_lookahead:a)", "(*sr:a)", "(*script_run:a)", "(?[a-z])"]
    # callouts
    out += ["(?C1)a(?C2)b(?C3)", "(a)(?C1)(b)(?C2)", "(a)|(b)(?C5)", "(a(?C1))+b", "(?C1)(a)(?C2)|(?C3)(b)(?C4)", "x(?C9)y|x(?C8)z", "(?:a(?C1)|b(?C2))*c",
            "(?<n>a)(?C7)", "(a)?(?C1)(b)?(?C2)", "^(?C1)word(?C2) ", "\\w+(?C3)\\s", ".*(?C1)a", "(?C255)a"]
    # classes in Unicode
    out += [r"\p{Greek}", r"\p{Latin}", r"\p{Lu}", r"\p{Han}", r"\P{Lu}", r"\p{^Lu}", r"\p{Any}", r"\p{L&}", r"\p{Xan}", r"\p{Xwd}", r"\p{Xsp}", r"\p{Xps}", r"\p{sc:Greek}", r"\p{scx:Greek}", r"\p{Bidi_Class:L}", r"\p{bc:L}",
            r"\p{Alphabetic}", r"\p{Script=Greek}", r"\pL+", r"[\p{L}\d]+", "\xe9", "[\xe9]", "\xc3\xa9", "[\xc3\xa9]", "(?i)\xc9", "(?i)\xc3\x89", r"\w+", r"\W+", r"\s+", r"\S+", r"\d+", r"\D+", r"\b\w+\b", r"(?i)k", r"(?i)\x{212a}", r"(?i)\x{e9}", r"(?i)\xe9",
            r"[[:upper:]]", r"(?i)[[:upper:]]", r"[[:^upper:]]", r"[[:word:]]", r"[[:space:]]", r"[[:blank:]]", r"[[:cntrl:]]", r"[[:graph:]]", r"[[:print:]]", r"[[:punct:]]", r"[[:xdigit:]]", r"[[:ascii:]]", r"[[:alnum:]]"]
    # backtracking, limits
    out += ["(a+)+$", "(a|aa)+$", "(.*)(.*)(.*)(.*)(.*)$", "^(?:a|b)*c$", "^(a?){20}a{20}$", "(x+x+)+y", "(a*)*b", "^(([a-z])+.)+[A-Z]([a-z])+$"]
    return out


def subjects():
    s = [b"j", b"y", b"xj", b"x{,3}", b"x{ 2 }", b"x{2, 3}", b"x{,}", b"x{}", b"xx", b"xxx", b"xxxx", b"x", b"", b"{,3}", b"{ 2 }", b"{", b"}", b"{}", b"ab{,3}", b"(x){,3}",
         b"-", b"_", b"a-b", b"-_", b"1-_", b"a_", b"\\", b"]", b"[", b"z", b"8", b"9", b"a8b", b"a9b", b"80", b"89", b"18", b"aaaaaaaa8", b"abcdefgh8", b"abcdefghh", b"8abcdefgh", b"abcdefgh",
         b"abcdefghii", b"abcdefghij", b"abcdefghijj", b"abcdefghia0", b"\n", b"\r\n", b"\r", b"a\nb", b"a\r\nb", b"a\rb", b"a\x85b", b"\x0b", b"\x0c", b"foobar", b"ab", b"abc", b"b", b"aab", b"aaab",
         b"aaaab", b"bcd", b"d", b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", b"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", b"Aa", b"AB", b"K", b"k", b"\xe2\x84\xaa", b"\xc3\xa9", b"\xc3\x89", b"\xe9", b"\xc9", b"\xce\xb1", b"\xce\xa9",
         b"\xe4\xb8\xad", b"\xe2\x82\xac", b"\xcd\x82", b"foo bar", b"The quick brown fox", b"A", b"Z", b"0", b"\x00", b"a\x00b", b"\x7f", b"\t", b" ", b"  "]
    return s
