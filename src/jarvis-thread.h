#ifndef JARVIS_THREAD_H
#define JARVIS_THREAD_H

#ifndef _WIN32
#include <pthread.h>
#include <time.h>

static inline void jarvis_clock_realtime(struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
}
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#ifndef ETIMEDOUT
#define ETIMEDOUT 138
#endif

typedef SRWLOCK jv_mutex_t;
typedef CONDITION_VARIABLE jv_cond_t;
typedef HANDLE jv_thread_t;

typedef struct {
	void *(*fn)(void *);
	void *arg;
} jv_thread_start_t;

static DWORD WINAPI jv_thread_trampoline(LPVOID param)
{
	jv_thread_start_t s = *(jv_thread_start_t *)param;
	free(param);
	(void)s.fn(s.arg);
	return 0;
}

static inline void jarvis_clock_realtime(struct timespec *ts)
{
	if (!ts) return;
	FILETIME ft;
	ULARGE_INTEGER uli;
	GetSystemTimeAsFileTime(&ft);
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	uint64_t ticks = uli.QuadPart - 116444736000000000ULL;
	ts->tv_sec = (time_t)(ticks / 10000000ULL);
	ts->tv_nsec = (long)((ticks % 10000000ULL) * 100ULL);
}

static inline int jv_mutex_lock(jv_mutex_t *m)
{
	AcquireSRWLockExclusive(m);
	return 0;
}

static inline int jv_mutex_unlock(jv_mutex_t *m)
{
	ReleaseSRWLockExclusive(m);
	return 0;
}

static inline int jv_cond_signal(jv_cond_t *c)
{
	WakeConditionVariable(c);
	return 0;
}

static inline int jv_cond_broadcast(jv_cond_t *c)
{
	WakeAllConditionVariable(c);
	return 0;
}

static inline int jv_cond_wait(jv_cond_t *c, jv_mutex_t *m)
{
	return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : EINVAL;
}

static inline int jv_cond_timedwait(jv_cond_t *c, jv_mutex_t *m, const struct timespec *abstime)
{
	DWORD wait_ms = 0;
	if (abstime) {
		struct timespec now;
		jarvis_clock_realtime(&now);
		int64_t sec = (int64_t)abstime->tv_sec - (int64_t)now.tv_sec;
		int64_t nsec = (int64_t)abstime->tv_nsec - (int64_t)now.tv_nsec;
		int64_t ms = sec * 1000LL + (nsec + 999999LL) / 1000000LL;
		if (ms > 0)
			wait_ms = (ms > 0x7FFFFFFELL) ? 0x7FFFFFFEUL : (DWORD)ms;
	}
	if (!SleepConditionVariableSRW(c, m, wait_ms, 0))
		return (GetLastError() == ERROR_TIMEOUT) ? ETIMEDOUT : EINVAL;
	return 0;
}

static inline int jv_thread_create(jv_thread_t *tid, const void *attr,
                                   void *(*fn)(void *), void *arg)
{
	(void)attr;
	jv_thread_start_t *s = (jv_thread_start_t *)malloc(sizeof(*s));
	if (!s) return ENOMEM;
	s->fn = fn;
	s->arg = arg;
	HANDLE h = CreateThread(NULL, 0, jv_thread_trampoline, s, 0, NULL);
	if (!h) {
		free(s);
		return EAGAIN;
	}
	if (tid) *tid = h;
	else CloseHandle(h);
	return 0;
}

static inline int jv_thread_join(jv_thread_t tid, void **retval)
{
	(void)retval;
	if (!tid) return EINVAL;
	WaitForSingleObject(tid, INFINITE);
	CloseHandle(tid);
	return 0;
}

static inline int jv_thread_detach(jv_thread_t tid)
{
	if (!tid) return EINVAL;
	CloseHandle(tid);
	return 0;
}

#ifdef pthread_mutex_t
#undef pthread_mutex_t
#endif
#ifdef pthread_cond_t
#undef pthread_cond_t
#endif
#ifdef pthread_t
#undef pthread_t
#endif
#define pthread_mutex_t           jv_mutex_t
#define pthread_cond_t            jv_cond_t
#define pthread_t                 jv_thread_t
#ifdef PTHREAD_MUTEX_INITIALIZER
#undef PTHREAD_MUTEX_INITIALIZER
#endif
#ifdef PTHREAD_COND_INITIALIZER
#undef PTHREAD_COND_INITIALIZER
#endif
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER  CONDITION_VARIABLE_INIT
#define pthread_mutex_lock        jv_mutex_lock
#define pthread_mutex_unlock      jv_mutex_unlock
#define pthread_cond_signal       jv_cond_signal
#define pthread_cond_broadcast    jv_cond_broadcast
#define pthread_cond_wait         jv_cond_wait
#define pthread_cond_timedwait    jv_cond_timedwait
#define pthread_create            jv_thread_create
#define pthread_join              jv_thread_join
#define pthread_detach            jv_thread_detach
#endif

#endif /* JARVIS_THREAD_H */
