/*
 * wincompat/poll.h — POSIX <poll.h> for the Windows port of kitty.
 *
 * struct pollfd and the POLL* constants come from <winsock2.h> (pulled in by
 * win_prelude.h). Winsock ships WSAPoll(), not POSIX poll(), so we declare
 * poll() here; it is implemented in wincompat.c (Stage 4) over a mix of socket
 * and pipe handles via WaitForMultipleObjects.
 */
#pragma once
#include <winsock2.h>

#ifndef _NFDS_T_DEFINED
#define _NFDS_T_DEFINED
typedef unsigned long nfds_t;
#endif

/* Implemented in wincompat/wincompat.c (Stage 4). */
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* ppoll() over poll(). The timeout is rounded up to whole milliseconds, since
 * truncating a short wait to zero turns the caller's sleep into a spin. There
 * are no signals to mask on Windows, so sigmask is ignored. */
#include <limits.h>
#include <time.h>
static inline int
ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *ts, const void *sigmask) {
    (void)sigmask;
    if (!ts) return poll(fds, nfds, -1);
    const long long ms = (long long)ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
    return poll(fds, nfds, ms > INT_MAX ? INT_MAX : (int)ms);
}
