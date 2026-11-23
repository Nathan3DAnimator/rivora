// A lock and a condition variable on every platform (Windows: SRW locks; Android and POSIX: pthreads), and a sleep in
// milliseconds. Header only: the files that need them include it (async.c, crowd.c, audio.c...).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK ReLock;
typedef CONDITION_VARIABLE ReCond;
#define RE_LOCK_INIT SRWLOCK_INIT
static inline void re_lock_init(ReLock* l) { InitializeSRWLock(l); }
static inline void re_lock(ReLock* l) { AcquireSRWLockExclusive(l); }
static inline void re_unlock(ReLock* l) { ReleaseSRWLockExclusive(l); }
static inline void re_cond_init(ReCond* c) { InitializeConditionVariable(c); }
static inline void re_cond_wait(ReCond* c, ReLock* l) { SleepConditionVariableSRW(c, l, INFINITE, 0); }
static inline void re_cond_wake_one(ReCond* c) { WakeConditionVariable(c); }
static inline void re_cond_wake_all(ReCond* c) { WakeAllConditionVariable(c); }
static inline long re_xchg32(volatile long* p, long v) { return InterlockedExchange(p, v); }
static inline void re_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t ReLock;
typedef pthread_cond_t ReCond;
#define RE_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void re_lock_init(ReLock* l) { pthread_mutex_init(l, NULL); }
static inline void re_lock(ReLock* l) { pthread_mutex_lock(l); }
static inline void re_unlock(ReLock* l) { pthread_mutex_unlock(l); }
static inline void re_cond_init(ReCond* c) { pthread_cond_init(c, NULL); }
static inline void re_cond_wait(ReCond* c, ReLock* l) { pthread_cond_wait(c, l); }
static inline void re_cond_wake_one(ReCond* c) { pthread_cond_signal(c); }
static inline void re_cond_wake_all(ReCond* c) { pthread_cond_broadcast(c); }
static inline long re_xchg32(volatile long* p, long v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline void re_sleep_ms(uint32_t ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif
