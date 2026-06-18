/* getopt_long() replacement for MSVC.
   Minimal implementation: supports long options with required_argument and no_argument.
   On MinGW (__GNUC__), the system getopt.h provides getopt_long. */
#ifndef COMPAT_GETOPT_H
#define COMPAT_GETOPT_H

#ifdef _MSC_VER

#include <string.h>
#include <stdio.h>

#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int         has_arg;
    int        *flag;
    int         val;
};

extern int opterr;
extern int optind;
extern int optopt;
extern char *optarg;

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#endif /* _MSC_VER */

#endif /* COMPAT_GETOPT_H */
