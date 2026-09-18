/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/cpus.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "system/cpu-timers.h"
#include "system/cpu-timers-internal.h"

int64_t cpu_get_clock_locked(void)
{
    int64_t time;

    time = timers_state.vm_clock_offset;
    if (timers_state.vm_clock_enabled) { time += get_clock(); }

    return time;
}

/*
 * Return the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
int64_t cpu_get_clock(void)
{
    int64_t  ti;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        ti    = cpu_get_clock_locked();
    }
    while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return ti;
}

/*
 * Start the VM clock.
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void vm_clock_enable(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
    if (!timers_state.vm_clock_enabled) {
        timers_state.vm_clock_offset  -= get_clock();
        timers_state.vm_clock_enabled  = 1;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
}

/*
 * Stop the VM clock. cpu_get_clock() keeps returning the time it stopped at.
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void vm_clock_disable(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
    if (timers_state.vm_clock_enabled) {
        timers_state.vm_clock_offset  = cpu_get_clock_locked();
        timers_state.vm_clock_enabled = 0;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
}

void qemu_timer_notify_cb(void* opaque, QEMUClockType type) { qemu_notify_event(); }

TimersState timers_state;

/* initialize timers state */
void cpu_timers_init(void)
{
    seqlock_init(&timers_state.vm_clock_seqlock);
    qemu_spin_init(&timers_state.vm_clock_lock);
}
