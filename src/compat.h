/* Cross-platform compatibility layer */
#ifndef COMPAT_H
#define COMPAT_H

#if defined(_WIN32) || defined(_WIN64)
/* ── Windows ──────────────────────────────────────────────── */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#ifdef __GNUC__
/* MinGW/MSYS2 provides POSIX headers */
#include <unistd.h>
#include <sys/stat.h>
#else
/* MSVC needs these for _mkdir, _getcwd, etc. */
#include <direct.h>
#endif

/* ── POSIX name mappings ────────────────────────────────────
 * MinGW/MSYS2 (__GNUC__) already provides standard POSIX names
 * via its headers. Only MSVC needs these ugly redefines. */
#ifndef __GNUC__
/* MSVC-only hacks */
#define sleep(s)          Sleep((s)*1000)
#define usleep(us)        Sleep((us)/1000)
/* NOTE: do NOT #define close → closesocket.
 * It breaks libwebsockets' internal struct member names
 * (LWS_FOP_CLOSE expands to 'close') and incorrectly maps
 * regular file close() to closesocket().
 * Use close_sock() to close sockets. */
#define strcasecmp        _stricmp
#define strncasecmp       _strnicmp
#define getcwd(b,s)       _getcwd(b,s)
#define chdir(p)          _chdir(p)
#define mkdir(p,m)        _mkdir(p)
#define unlink(p)         _unlink(p)
#define stat(p,s)         _stat(p,s)
#define fstat(f,s)        _fstat(f,s)
#define lstat(p,s)        _stat(p,s)  /* no symlinks on Windows */
#endif  /* !__GNUC__ */

/* MinGW has stat() but not lstat(). Windows has no symlinks. */
#ifdef __GNUC__
#define lstat(p,s)  stat(p,s)
#endif

typedef int socklen_t;
typedef SOCKET socket_t;
#define INVALID_FD        INVALID_SOCKET
#define SOCKET_ERROR_VAL  SOCKET_ERROR

/* gettimeofday replacement (MSVC only — MinGW has it in sys/time.h) */
#ifndef __GNUC__
#include <sys/timeb.h>
static inline int compat_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    struct _timeb tb;
    _ftime(&tb);
    tv->tv_sec = (long)tb.time;
    tv->tv_usec = tb.millitm * 1000;
    return 0;
}
#define gettimeofday compat_gettimeofday
#endif

/* No getopt_long on MSVC — use a simple shim for MinGW; MSVC needs replacement */
#ifndef __GNUC__
/* MSVC: minimal getopt replacement */
extern int optind;
extern char *optarg;
int getopt(int argc, char *const argv[], const char *optstring);
#else
/* MinGW has getopt_long */
#include <getopt.h>
#endif

/* Socket startup/shutdown */
static inline int net_startup(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2,2), &wsa);
}
static inline void net_cleanup(void) { WSACleanup(); }
#define SOCKET_INIT()    net_startup()
#define SOCKET_QUIT()    net_cleanup()

/* Non-blocking socket setup */
static inline int sock_set_nonblock(socket_t fd) {
    unsigned long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
}

/* Portable socket close (closesocket on Win, close on POSIX) */
static inline int close_sock(socket_t fd) {
    return closesocket(fd);
}

/* Portable setsockopt/getsockopt for integer values.
 * Windows' Winsock expects (const char *) for optval,
 * while POSIX expects (const void *). The cast works on both. */
static inline int sock_setopt_int(socket_t fd, int level, int optname, int val) {
    return setsockopt(fd, level, optname, (const char *)&val, sizeof(val));
}
static inline int sock_getopt_int(socket_t fd, int level, int optname, int *val) {
    socklen_t len = sizeof(*val);
    return getsockopt(fd, level, optname, (char *)val, &len);
}

/* Portable socket I/O — Windows needs send/recv, POSIX uses write/read */
static inline ssize_t sock_write(socket_t fd, const void *buf, size_t len) {
    return (ssize_t)send(fd, (const char *)buf, (int)len, 0);
}
static inline ssize_t sock_read(socket_t fd, void *buf, size_t len) {
    return (ssize_t)recv(fd, (char *)buf, (int)len, 0);
}

/* Missing POSIX types (MSVC only — MinGW provides them) */
#ifndef __GNUC__
#define ssize_t SSIZE_T
#endif

#else
/* ── Linux / Unix ─────────────────────────────────────────── */
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <getopt.h>

typedef int socket_t;
#define INVALID_FD        (-1)
#define SOCKET_ERROR_VAL  (-1)
#define SOCKET_INIT()     0
#define SOCKET_QUIT()     do{}while(0)

static inline int sock_set_nonblock(socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int close_sock(socket_t fd) {
    return close(fd);
}

static inline int sock_setopt_int(socket_t fd, int level, int optname, int val) {
    return setsockopt(fd, level, optname, &val, sizeof(val));
}
static inline int sock_getopt_int(socket_t fd, int level, int optname, int *val) {
    socklen_t len = sizeof(*val);
    return getsockopt(fd, level, optname, val, &len);
}

static inline ssize_t sock_write(socket_t fd, const void *buf, size_t len) {
    return write(fd, buf, len);
}
static inline ssize_t sock_read(socket_t fd, void *buf, size_t len) {
    return read(fd, buf, len);
}
#endif

#endif /* COMPAT_H */
