/*
 * Force-included (-include) into every translation unit of the mingw-w64
 * build. Upstream builds LinBPQ.exe with MSVC; GCC lacks two MSVC-isms the
 * sources rely on.
 */
#ifndef WINSHIM_H
#define WINSHIM_H

/*
 * GCC has no structured exception handling. A plain block with a dead
 * handler is what the Linux build compiles, where these blocks are absent:
 * a fault terminates the process instead of being logged and survived.
 */
#define __try if (1)
#define __except(...) else if (0)

/*
 * compatbits.c defines pthread_equal() for MSVC, which has no pthreads.
 * mingw-w64's winpthreads, linked statically, defines it too.
 */
#define pthread_equal bpq_pthread_equal

#endif
