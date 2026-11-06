#pragma once
#include "local_lock.h"

/* Device lock storage must survive reset/reinitialization. IRQ handlers use
 * their separate leaf locks and must never acquire a device/local lock. */
#define DEVICE_LOCK_GUARD(lock) LOCAL_LOCK_NAMED(lock, device_guard, 0)
#define DEVICE_TRY_GUARD(lock) LOCAL_LOCK_NAMED(lock, device_guard, 1)
