zlib built as follows:

1. Download recent version from https://www.zlib.net/zlib132.zip

2. Unzip and untar the file

3. Copy the following files to the mushclient source "zlib" directory:

adler32.c
compress.c
crc32.c
crc32.h
deflate.c
deflate.h
gzclose.c
gzguts.h
gzlib.c
gzread.c
gzwrite.c
inffast.c
inffast.h
inffixed.h
inflate.c
inflate.h
inftrees.c
inftrees.h
trees.c
trees.h
uncompr.c
zconf.h
zlib.h
zutil.c
zutil.h


3. Edit: zconf.h and after the lines:

#ifndef ZCONF_H
#define ZCONF_H

add:

#undef ZLIB_DLL      // NJG


4. Edit gzguts.h and after the lines:

#if defined(_WIN32) && !defined(WIDECHAR)
#  define WIDECHAR
#endif

add:

#if defined(_WIN32)     /* MUSHclient: the ISO names, see readme.txt */
#  define open _open
#  define read _read
#  define write _write
#  define close _close
#endif
