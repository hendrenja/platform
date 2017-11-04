/* Copyright (c) 2010-2017 the corto developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <corto/base.h>

corto_thread corto_thread_new(corto_thread_cb f, void* arg) {
    pthread_t thread;
    int r;

    if ((r = pthread_create (&thread, NULL, f, arg))) {
        corto_critical("pthread_create failed: %d", r);
    }

    return (corto_thread)thread;
}

int corto_thread_join(corto_thread thread, void** result) {
    return pthread_join((pthread_t)thread, result);
}

int corto_thread_detach(corto_thread thread) {
    pthread_detach((pthread_t)thread);
    return 0;
}

int corto_thread_setPriority(corto_thread thread, int priority) {
    pthread_t tid;
    struct sched_param param;

    tid = (pthread_t)thread;

    param.sched_priority = priority;

    return pthread_setschedparam(tid, SCHED_OTHER, &param);
}

int corto_thread_getPriority(corto_thread thread) {
    pthread_t tid;
    int policy;
    struct sched_param param;

    tid = (pthread_t)thread;

    if (pthread_getschedparam(tid, &policy, &param)) {
        return -1;
    }

    return param.sched_priority;
}

corto_thread corto_thread_self() {
    return (corto_thread)pthread_self();
}

/* Keep track of registered thread keys since pthread does not call destructors
 * for the main thread */
typedef struct corto_threadTlsAdmin_t {
    corto_tls key;
    void (*destructor)(void*);
} corto_threadTlsAdmin_t;

static corto_threadTlsAdmin_t corto_threadTlsAdmin[CORTO_MAX_THREAD_KEY];
static int32_t corto_threadTlsCount = -1;

int corto_tls_new(corto_tls* key, void(*destructor)(void*)){
    if (pthread_key_create(key, destructor)) {
        corto_throw("corto_tls_new failed.");
        goto error;
    }

    if (destructor) {
        int32_t slot = corto_ainc(&corto_threadTlsCount);
        corto_threadTlsAdmin[slot].key = *key;
        corto_threadTlsAdmin[slot].destructor = destructor;
    }

    return 0;
error:
    return -1;
}

int corto_tls_set(corto_tls key, void* value) {
    return pthread_setspecific(key, value);
}

void* corto_tls_get(corto_tls key) {
    return pthread_getspecific(key);
}

void corto_tls_free(void) {
    int32_t i;

    for (i = 0; i <= corto_threadTlsCount; i++) {
        void *data = corto_tls_get(corto_threadTlsAdmin[i].key);
        corto_threadTlsAdmin[i].destructor(data);
    }
}

int corto_mutex_new(struct corto_mutex_s *m) {
    int result;
    if ((result = pthread_mutex_init (&m->mutex, NULL))) {
        corto_throw("mutexNew failed: %s", strerror(result));
    }
    return result;
}

int corto_mutex_lock(corto_mutex mutex) {
    int result;
    if ((result = pthread_mutex_lock(&mutex->mutex))) {
        corto_throw("mutexLock failed: %s", strerror(result));
    }
    return result;
}

int corto_mutex_unlock(corto_mutex mutex) {
    return pthread_mutex_unlock(&mutex->mutex);
}

int corto_mutex_free(corto_mutex mutex) {
    int result;
    if ((result = pthread_mutex_destroy(&mutex->mutex))) {
        corto_throw("mutexFree failed: %s", strerror(result));
    }
    return result;
}

int corto_mutex_try(corto_mutex mutex) {
    int result;
    if ((result = pthread_mutex_trylock(&mutex->mutex))) {
        corto_throw("mutexTry failed: %s", strerror(result));
    }
    return result;
}

// OS X does not have pthread_mutex_timedlock
#ifdef __MACH__
static int pthread_mutex_timedlock (
    pthread_mutex_t *mutex,
    struct timespec *timeout)
{
   corto_time timenow;
   struct timespec sleeptime;
   int retcode;

   sleeptime.tv_sec = 0;
   sleeptime.tv_nsec = 10000000; /* 10ms */

   while ((retcode = pthread_mutex_trylock (mutex)) == EBUSY) {
      corto_timeGet (&timenow);

      if (timenow.sec >= timeout->tv_sec &&
         (timenow.nanosec * 1000) >= timeout->tv_nsec)
      {
          return ETIMEDOUT;
      }

      nanosleep (&sleeptime, NULL);
   }

   return retcode;
}
#endif

int corto_mutex_lockTimed(corto_mutex mutex, struct timespec ts) {
    int result;

    if ((result = pthread_mutex_timedlock(&mutex->mutex, &ts))) {
        if (result != ETIMEDOUT) {
            corto_throw("mutexTry failed: %s", strerror(result));
        }
    }

    return result;
}

/* Create read-write mutex */
int corto_rwmutex_new(struct corto_rwmutex_s *m) {
    int result = 0;

    if ((result = pthread_rwlock_init(&m->mutex, NULL))) {
        corto_throw("rwmutexNew failed: %s", strerror(result));
    }
    return result;
}

/* Free read-write mutex */
int corto_rwmutex_free(corto_rwmutex m) {
    int result = 0;
    if ((result = pthread_rwlock_destroy(&m->mutex))) {
        corto_throw("rwmutexFree failed: %s", strerror(result));
    }
    return result;
}

/* Read lock */
int corto_rwmutex_read(corto_rwmutex mutex) {
    int result = 0;
    if ((result = pthread_rwlock_rdlock(&mutex->mutex))) {
        corto_throw("rwmutexRead failed: %s", strerror(result));
    }
    return result;
}

/* Write lock */
int corto_rwmutex_write(corto_rwmutex mutex) {
    int result = 0;
    if ((result = pthread_rwlock_wrlock(&mutex->mutex))) {
        corto_throw("rwmutexWrite failed: %s", strerror(result));
    }
    return result;
}

/* Try read */
int corto_rwmutex_tryRead(corto_rwmutex mutex) {
    int result;
    if ((result = pthread_rwlock_tryrdlock(&mutex->mutex))) {
        corto_throw("rwmutexTryRead failed: %s", strerror(result));
    }
    return result;
}

/* Try write */
int corto_rwmutex_tryWrite(corto_rwmutex mutex) {
    int result;
    if ((result = pthread_rwlock_trywrlock(&mutex->mutex))) {
        corto_throw("rwmutexTryWrite failed: %s", strerror(result));
    }
    return result;
}

/* Write unlock */
int corto_rwmutex_unlock(corto_rwmutex mutex) {
    int result;
    if ((result = pthread_rwlock_unlock(&mutex->mutex))) {
        corto_throw("rwmutexUnlock failed: %s", strerror(result));
    }
    return result;
}

corto_sem corto_sem_new(unsigned int initValue) {
    corto_sem semaphore;

    semaphore = malloc (sizeof(corto_sem_s));
    memset(semaphore, 0, sizeof(corto_sem_s));
    if (semaphore) {
        pthread_mutex_init(&semaphore->mutex, NULL);
        pthread_cond_init(&semaphore->cond, NULL);
        semaphore->value = initValue;
    }

    return (corto_sem)semaphore;
}

/* Post to semaphore */
int corto_sem_post(corto_sem semaphore) {
    pthread_mutex_lock(&semaphore->mutex);
    semaphore->value++;
    if(semaphore->value > 0) {
        pthread_cond_signal(&semaphore->cond);
    }
    pthread_mutex_unlock(&semaphore->mutex);
    return 0;
}

/* Wait for semaphore */
int corto_sem_wait(corto_sem semaphore) {
    pthread_mutex_lock(&semaphore->mutex);
    while(semaphore->value <= 0) {
        pthread_cond_wait(&semaphore->cond, &semaphore->mutex);
    }
    semaphore->value--;
    pthread_mutex_unlock(&semaphore->mutex);
    return 0;
}

/* Trywait for semaphore */
int corto_sem_tryWait(corto_sem semaphore) {
    int result;

    pthread_mutex_lock(&semaphore->mutex);
    if(semaphore->value > 0) {
        semaphore->value--;
        result = 0;
    } else {
        errno = EAGAIN;
        result = -1;
    }
    pthread_mutex_unlock(&semaphore->mutex);
    return result;
}

/* Get value of semaphore */
int corto_sem_value(corto_sem semaphore) {
    return semaphore->value;
}

/* Free semaphore */
int corto_sem_free(corto_sem semaphore) {
    pthread_cond_destroy(&semaphore->cond);
    pthread_mutex_destroy(&semaphore->mutex);
    free(semaphore);
    return 0;
}


int corto_ainc(int* count) {
    int value;
#ifdef __GNUC__
    /*__asm__ __volatile__ (
            "movl $1, %0\n\t"
            "lock\n\t"
            "xaddl %0, %2\n\t"
            "incl %0"
    :       "=&r" (value), "=m" (*count)
    :       "m" (*count)
    :       "memory");*/
    value = __sync_add_and_fetch (count, 1);
    return value;
#else
    AtomicModify( count, &value, 0, 1 );
    return( value + 1 );
#endif
}

int corto_adec(int* count) {
    int value;
#ifdef __GNUC__
    /*__asm__ __volatile__ (
            "movl $-1, %0\n\t"
            "lock\n\t"
            "xaddl %0, %2\n\t"
            "decl %0"
    :       "=&r" (value), "=m" (*count)
    :       "m" (*count)
    :       "memory");*/
    value = __sync_sub_and_fetch (count, 1);
    return value;
#else
    AtomicModify( count, &value, 0, -1 );
    return( value - 1 );
#endif
}
