/* getopt_long() implementation for MSVC.
   Based on public-domain musl getopt, extended with long option support. */
#ifdef _MSC_VER

#include "compat_getopt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int opterr = 1;
int optind = 1;
int optopt = 0;
char *optarg = NULL;

static int optpos = 0;

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    if (longindex) *longindex = -1;

    if (optind >= argc) return -1;

    const char *arg = argv[optind];

    /* -- option */
    if (arg[0] == '-' && arg[1] == '-') {
        if (arg[2] == '\0') { optind++; return -1; } /* -- */

        const char *eq = strchr(arg + 2, '=');
        size_t namelen = eq ? (size_t)(eq - (arg + 2)) : strlen(arg + 2);

        for (int i = 0; longopts && longopts[i].name; i++) {
            if (strncmp(arg + 2, longopts[i].name, namelen) == 0 &&
                longopts[i].name[namelen] == '\0') {
                if (longindex) *longindex = i;

                if (longopts[i].flag) {
                    *longopts[i].flag = longopts[i].val;
                    optind++;
                    return 0;
                }

                if (longopts[i].has_arg == required_argument) {
                    if (eq) {
                        optarg = (char *)(eq + 1);
                    } else if (optind + 1 < argc) {
                        optarg = argv[++optind];
                    } else {
                        if (opterr)
                            fprintf(stderr, "%s: option '--%s' requires an argument\n",
                                    argv[0], longopts[i].name);
                        optind++;
                        return '?';
                    }
                    optind++;
                    return longopts[i].val;
                }
                /* no_argument */
                optind++;
                return longopts[i].val;
            }
        }
        if (opterr)
            fprintf(stderr, "%s: unrecognized option '--%s'\n", argv[0], arg + 2);
        optind++;
        return '?';
    }

    /* - short option */
    if (arg[0] == '-' && arg[1] != '\0') {
        if (optpos == 0) optpos = 1;
        char c = arg[optpos++];
        const char *p = strchr(optstring, c);
        if (!p) {
            if (opterr)
                fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
            optopt = c;
            if (arg[optpos] == '\0') { optind++; optpos = 0; }
            return '?';
        }
        if (p[1] == ':') {
            if (arg[optpos] != '\0') {
                optarg = (char *)(arg + optpos);
            } else if (++optind < argc) {
                optarg = argv[optind];
            } else {
                if (opterr)
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
                optopt = c;
                optind++;
                optpos = 0;
                return '?';
            }
            optind++;
            optpos = 0;
        }
        if (arg[optpos] == '\0') { optind++; optpos = 0; }
        return c;
    }

    return -1;
}
#endif /* _MSC_VER */
