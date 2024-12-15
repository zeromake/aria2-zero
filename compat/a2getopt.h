#ifndef _A2GETOPT_H_
#define _A2GETOPT_H_
#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_GETOPT_H
#  include <getopt.h>
#else
#  include "_getopt.h"
#endif

#ifdef __cplusplus
}
#endif
#endif // !_A2GETOPT_H_
