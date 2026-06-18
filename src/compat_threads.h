/* pthread → Win32 threads compat layer for MSVC.
   On MinGW (__GNUC__), native pthread.h is used.
   On MSVC (_MSC_VER), Windows threads are wrapped. */
#ifndef COMPAT_THREADS_H
#define COMPAT_THREADS_H

#ifdef _MSC_VER

#include <windows.h>
#include <process.h>

typedef HANDLE pthread_t;

typedef void *(*pthread_func_t)(void *);

struct pthread_create_args {
    pthread_func_t func;
    void *arg;
};

static unsigned int __stdcall pthread_thunk(void *p)
{
    struct pthread_create_args *a = (struct pthread_create_args *)p;
    pthread_func_t fn = a->func;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr,
                                 pthread_func_t func, void *arg)
{
    (void)attr;
    struct pthread_create_args *a = malloc(sizeof(*a));
    if (!a) return -1;
    a->func = func;
    a->arg = arg;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, pthread_thunk, a, 0, NULL);
    if (!h) { free(a); return -1; }
    *thread = h;
    return 0;
}

static inline int pthread_detach(pthread_t thread)
{
    CloseHandle(thread);
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    if (retval) *retval = NULL;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

#else
#include <pthread.h>
#endif /* _MSC_VER */

#endif /* COMPAT_THREADS_H */
