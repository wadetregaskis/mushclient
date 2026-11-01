PCRE2 10.49 (https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.49), for
triggers, aliases, the Find dialogs and Lua's "rex".  See LICENCE.md.

The client does not call it directly except through src/mc_pcre.c, which keeps PCRE
8's option numbers, error numbers, and int offsets (src/mc_pcre.h says why) and
rewrites the few patterns PCRE2 would refuse or read differently.
tools/pcre_compare compares the result with PCRE 8.45.


BUILDING A NEW VERSION
----------------------

1. Download the release's pcre2-<version>.tar.gz, and check its SHA-256 against the
   digest on the release page.

2. Copy to pcre2\src:

     src\pcre2_*.c and src\pcre2_*.h, except pcre2_dftables.c, pcre2_fuzzsupport.c
       and pcre2_jit_test.c
     src\pcre2.h.generic as pcre2.h
     src\pcre2_chartables.c.dist as pcre2_chartables.c

   and to pcre2\deps\sljit: LICENSE, README.md and the whole of sljit_src.
   Copy LICENCE.md to pcre2.  Nothing else is changed.

3. pcre2\src\config.h is ours.  Compare it with the new config.h.generic.

4. Any new .c file goes in MUSHclient.vcxproj (with HAVE_CONFIG_H, and not using the
   precompiled header) and CMakeLists.txt, and then run tests\win\gen_test_vcxproj.ps1.

5. Run tools/pcre_compare, and the regexp tests.
