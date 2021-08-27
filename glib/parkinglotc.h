#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

void plc_cond_init(uintptr_t *cond);

void plc_cond_clear(uintptr_t *cond);

bool plc_cond_wait(uintptr_t *cond, uintptr_t *mutex);

bool plc_cond_wait_for(uintptr_t *cond,
                       uintptr_t *mutex,
                       uint64_t duration_s,
                       uint64_t duration_part_ns);

bool plc_cond_notify_one(uintptr_t *cond);

uintptr_t plc_cond_notify_all(uintptr_t *cond);

void plc_mutex_init(uintptr_t *mutex);

void plc_mutex_clear(uintptr_t *mutex);

void plc_mutex_lock(uintptr_t *mutex);

void plc_mutex_unlock(uintptr_t *mutex);

bool plc_mutex_try_lock(uintptr_t *mutex);

void plc_rmutex_init(uintptr_t *mutex);

void plc_rmutex_clear(uintptr_t *mutex);

void plc_rmutex_lock(uintptr_t *mutex);

void plc_rmutex_unlock(uintptr_t *mutex);

bool plc_rmutex_try_lock(uintptr_t *mutex);

void plc_rwlock_init(uintptr_t *rwlock);

void plc_rwlock_clear(uintptr_t *rwlock);

void plc_rwlock_lock_exclusive(uintptr_t *rwlock);

void plc_rwlock_unlock_exclusive(uintptr_t *rwlock);

bool plc_rwlock_try_lock_exclusive(uintptr_t *rwlock);

void plc_rwlock_lock_shared(uintptr_t *rwlock);

void plc_rwlock_unlock_shared(uintptr_t *rwlock);

bool plc_rwlock_try_lock_shared(uintptr_t *rwlock);
