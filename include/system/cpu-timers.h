/*
 * CPU timers state API
 *
 * Copyright 2020 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#pragma once

#include "qemu/timer.h"

/* init the whole cpu timers API */
void cpu_timers_init(void);

/*
 * VM clock
 */

/* Caller must hold BQL */
void vm_clock_enable(void);
/* Caller must hold BQL */
void vm_clock_disable(void);

/*
 * Returns the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
int64_t cpu_get_clock(void);

void qemu_timer_notify_cb(void* opaque, QEMUClockType type);

/* get VIRTUAL clock via the cpus accel interface */
int64_t cpus_get_virtual_clock(void);
