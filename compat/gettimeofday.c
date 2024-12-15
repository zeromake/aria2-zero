/* source:
 * cygwin-1.5.24-2-src/cygwin-1.5.24-2/winsup/mingw/mingwex/gettimeofday.c */

/*
 * gettimeofday
 * Implementation according to:
 * The Open Group Base Specifications Issue 6
 * IEEE Std 1003.1, 2004 Edition
 */

/*
 *  THIS SOFTWARE IS NOT COPYRIGHTED
 *
 *  This source code is offered for use in the public domain. You may
 *  use, modify or distribute it freely.
 *
 *  This code is distributed in the hope that it will be useful but
 *  WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 *  DISCLAIMED. This includes but is not limited to warranties of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *  Contributed by:
 *  Danny Smith <dannysmith@users.sourceforge.net>
 */

#include "gettimeofday.h"
#include <WinSock2.h>
#include <windows.h>

/* Offset between 1/1/1601 and 1/1/1970 in 100 nanosec units */
#define _W32_FT_OFFSET (116444736000000000ULL)

int gettimeofday(struct timeval* tp, void* tzp)
{
  union {
    unsigned long long ns100;
    FILETIME ft;
  } _now;

  if (tp) {
    GetSystemTimeAsFileTime(&_now.ft);
    tp->tv_usec = (long)((_now.ns100 / 10ULL) % 1000000ULL);
    tp->tv_sec = (long)((_now.ns100 - _W32_FT_OFFSET) / 10000000ULL);
  }
  return 0;
}
