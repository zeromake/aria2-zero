#ifndef PORTABLE_STRNCASECMP_H_
#define PORTABLE_STRNCASECMP_H_

#ifndef HAVE_STRNCASECMP
#  include <stddef.h>
int strncasecmp(const char* s1, const char* s2, size_t n);
#endif

#endif