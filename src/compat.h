/* mvci_compat.h — tiny cross-platform layer: timing, mutex, thread. */
#ifndef MVCI_COMPAT_H
#define MVCI_COMPAT_H

#include <stdint.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>

  typedef CRITICAL_SECTION mvci_mutex_t;
  static __inline void mvci_mutex_init(mvci_mutex_t *m)    { InitializeCriticalSection(m); }
  static __inline void mvci_mutex_lock(mvci_mutex_t *m)    { EnterCriticalSection(m); }
  static __inline void mvci_mutex_unlock(mvci_mutex_t *m)  { LeaveCriticalSection(m); }
  static __inline void mvci_mutex_destroy(mvci_mutex_t *m) { DeleteCriticalSection(m); }

  static __inline void     mvci_sleep_ms(int ms) { Sleep((DWORD)ms); }
  static __inline uint32_t mvci_now_ms(void)     { return (uint32_t)GetTickCount(); }

  typedef HANDLE mvci_thread_t;
  #define MVCI_THREAD_RET unsigned long __stdcall
  typedef unsigned long (__stdcall *mvci_thread_fn)(void *);
  static __inline int  mvci_thread_create(mvci_thread_t *t, mvci_thread_fn fn, void *arg) {
      *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
      return *t ? 0 : -1;
  }
  static __inline void mvci_thread_join(mvci_thread_t t) {
      if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
  }
#else
  #include <pthread.h>
  #include <time.h>

  typedef pthread_mutex_t mvci_mutex_t;
  static inline void mvci_mutex_init(mvci_mutex_t *m)    { pthread_mutex_init(m, NULL); }
  static inline void mvci_mutex_lock(mvci_mutex_t *m)    { pthread_mutex_lock(m); }
  static inline void mvci_mutex_unlock(mvci_mutex_t *m)  { pthread_mutex_unlock(m); }
  static inline void mvci_mutex_destroy(mvci_mutex_t *m) { pthread_mutex_destroy(m); }

  static inline void mvci_sleep_ms(int ms) {
      struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
      nanosleep(&t, NULL);
  }
  static inline uint32_t mvci_now_ms(void) {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
  }

  typedef pthread_t mvci_thread_t;
  #define MVCI_THREAD_RET void *
  typedef void *(*mvci_thread_fn)(void *);
  static inline int  mvci_thread_create(mvci_thread_t *t, mvci_thread_fn fn, void *arg) {
      return pthread_create(t, NULL, fn, arg);
  }
  static inline void mvci_thread_join(mvci_thread_t t) { pthread_join(t, NULL); }
#endif

#endif /* MVCI_COMPAT_H */
