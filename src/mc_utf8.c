/* mc_utf8.c -- the UTF-8 helpers the client long borrowed from PCRE 8.

   utils, the telnet layer, miniwindows and the trigger path all use these, under
   the names PCRE 8 gave them, and scripts can see what they decide (a bad string's
   column, for one).  PCRE2's own are private and differ in detail, so they stay
   as they were: from PCRE 8.45's pcre_valid_utf8.c, pcre_ord2utf8.c and
   pcre_tables.c, by Philip Hazel, Copyright (c) 1997-2021 University of Cambridge,
   under the BSD licence in "licences/PCRE licence.txt". */

/* Number of additional bytes after a UTF-8 lead byte, indexed by its low six bits */
const unsigned char _pcre_utf8_table4 [] = {
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  3,3,3,3,3,3,3,3,4,4,4,4,5,5,5,5 };

/* Encode a character value, up to 0x7fffffff, in 1 to 6 bytes: returns how many */
int _pcre_ord2utf (int value, unsigned char * buffer)
  {
  static const int table1 [] = { 0x7f, 0x7ff, 0xffff, 0x1fffff, 0x3ffffff, 0x7fffffff };
  static const int table2 [] = { 0, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc };
  unsigned int cvalue = (unsigned int) value;
  int i, j;

  for (i = 0; i < 6; i++)
    if ((int) cvalue <= table1 [i])
      break;
  buffer += i;
  for (j = i; j > 0; j--)
    {
    *buffer-- = (unsigned char) (0x80 | (cvalue & 0x3f));
    cvalue >>= 6;
    }
  *buffer = (unsigned char) (table2 [i] | cvalue);
  return i + 1;
  }

/* Validate a UTF-8 string (RFC 3629).  length < 0: zero-terminated.  Returns 0 if
   good, otherwise PCRE 8's PCRE_UTF8_ERRn, with the offset of the bad character:

    1-5  n bytes missing at the end        6-10 byte 2-6's top bits are not 10
    11   5-byte character                  12   6-byte character
    13   value above 0x10ffff              14   value in 0xd800-0xdfff
    15-19 overlong 2-6 byte sequence       20   isolated 10xx xxxx byte
    21   byte 0xfe or 0xff */
int _pcre_valid_utf (const unsigned char * string, int length, int * erroroffset)
  {
  const unsigned char * p;

  if (length < 0)
    {
    for (p = string; *p != 0; p++)
      ;
    length = (int) (p - string);
    }

  for (p = string; length-- > 0; p++)
    {
    unsigned char ab, c, d;

    c = *p;
    if (c < 128)
      continue;                         /* ASCII character */

    if (c < 0xc0)                       /* Isolated 10xx xxxx byte */
      {
      *erroroffset = (int) (p - string);
      return 20;
      }

    if (c >= 0xfe)                      /* Invalid 0xfe or 0xff bytes */
      {
      *erroroffset = (int) (p - string);
      return 21;
      }

    ab = _pcre_utf8_table4 [c & 0x3f];  /* Number of additional bytes */
    if (length < ab)
      {
      *erroroffset = (int) (p - string);   /* Missing bytes */
      return ab - length;                  /* Codes ERR1 to ERR5 */
      }
    length -= ab;                       /* Length remaining */

    /* Check top bits in the second byte */

    if (((d = *(++p)) & 0xc0) != 0x80)
      {
      *erroroffset = (int) (p - string) - 1;
      return 6;
      }

    /* For each length, check that the remaining bytes start with the 0x80 bit
       set and not the 0x40 bit. Then check for an overlong sequence, and for the
       excluded range 0xd800 to 0xdfff. */

    switch (ab)
      {
      case 1:                           /* 2 bytes */
        if ((c & 0x3e) == 0)
          {
          *erroroffset = (int) (p - string) - 1;
          return 15;
          }
        break;

      case 2:                           /* 3 bytes */
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 2;
          return 7;
          }
        if (c == 0xe0 && (d & 0x20) == 0)
          {
          *erroroffset = (int) (p - string) - 2;
          return 16;
          }
        if (c == 0xed && d >= 0xa0)
          {
          *erroroffset = (int) (p - string) - 2;
          return 14;
          }
        break;

      case 3:                           /* 4 bytes */
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 2;
          return 7;
          }
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 3;
          return 8;
          }
        if (c == 0xf0 && (d & 0x30) == 0)
          {
          *erroroffset = (int) (p - string) - 3;
          return 17;
          }
        if (c > 0xf4 || (c == 0xf4 && d > 0x8f))
          {
          *erroroffset = (int) (p - string) - 3;
          return 13;
          }
        break;

      /* 5-byte and 6-byte characters are not allowed by RFC 3629, and are rejected
         below, but overlong sequences get diagnosed first. */

      case 4:                           /* 5 bytes */
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 2;
          return 7;
          }
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 3;
          return 8;
          }
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 4;
          return 9;
          }
        if (c == 0xf8 && (d & 0x38) == 0)
          {
          *erroroffset = (int) (p - string) - 4;
          return 18;
          }
        break;

      case 5:                           /* 6 bytes */
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 2;
          return 7;
          }
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 3;
          return 8;
          }
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 4;
          return 9;
          }
        if ((*(++p) & 0xc0) != 0x80)
          {
          *erroroffset = (int) (p - string) - 5;
          return 10;
          }
        if (c == 0xfc && (d & 0x3c) == 0)
          {
          *erroroffset = (int) (p - string) - 5;
          return 19;
          }
        break;
      }

    /* Valid under RFC 2279, but RFC 3629 excludes 5 and 6 bytes.  p is at the
       last byte of the character. */

    if (ab > 3)
      {
      *erroroffset = (int) (p - string) - ab;
      return ab == 4 ? 11 : 12;
      }
    }

  return 0;
  }
