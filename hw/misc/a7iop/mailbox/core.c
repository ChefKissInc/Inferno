/*
 * Apple A7IOP Mailbox.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "block/aio.h"
#include "hw/irq.h"
#include "hw/misc/a7iop/base.h"
#include "hw/misc/a7iop/mailbox/core.h"
#include "hw/misc/a7iop/mailbox/private.h"
#include "hw/misc/a7iop/mailbox/trace.h"
#include "hw/misc/a7iop/private.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"
#include "qemu/bitops.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"

#define CTRL_ENABLE      BIT(0)
#define CTRL_FULL        BIT(16)
#define CTRL_EMPTY       BIT(17)
#define CTRL_OVERFLOW    BIT(18)
#define CTRL_UNDERFLOW   BIT(19)
#define CTRL_COUNT_SHIFT (20)
#define CTRL_COUNT_MASK  (A7IOP_MAX_MESSAGES << CTRL_COUNT_SHIFT)
#define CTRL_COUNT(v)    (((v) << CTRL_COUNT_SHIFT) & CTRL_COUNT_MASK)

static bool is_interrupt_enabled(AppleA7IOPMailbox* mailbox, uint32_t status)
{
    if (!mailbox->sepd_enabled) {
        // this workaround, just like the others, also seems to have the
        // side-effect of reducing the amount of timer0 interrupts, at least
        // under 18.5.
        return false;
    }
    if ((status & 0xF0000) == 0x10000 && (mailbox->glb_cfg & KIC_GLB_CFG_EXT_INT_EN) != 0) {
        uint32_t interrupt       = status & 0x7F;
        int      interrupt_group = interrupt / 32;
        // assert_cmpuint(interrupt_group, <, 4);
        uint32_t interrupt_enabled = mailbox->interrupts_enabled[interrupt_group] & BIT(interrupt % 32);
        if (interrupt_enabled) {
            // if (status == INTERRUPT_SEP_MANUAL_TIMER) {
            //     // if timer0 or timer1 (maybe only timer0) unmasked, possible workaround for sepfw 18.5
            //     exception.c:69, and it might not massively change the timer0 [sic] behavior, but only slightly
            //     // this approach doesn't fix 18.5
            //     if (!mailbox->timer0_masked || !mailbox->timer1_masked)
            //         return false;
            // }
            return true;
        }
    }
    else if (status == IRQ_IOP_NONEMPTY) {
        if (mailbox->iop_nonempty) { return true; }
    }
    else if (status == IRQ_IOP_EMPTY) {
        if (mailbox->iop_empty) { return true; }
    }
    else if (status == IRQ_AP_NONEMPTY) {
        if (mailbox->ap_nonempty) { return true; }
    }
    else if (status == IRQ_AP_EMPTY) {
        if (mailbox->ap_empty) { return true; }
    }
    else if (status == IRQ_SEP_TIMER0) {
        // fprintf(stderr, "%s: IRQ_SEP_TIMER0: status: 0x%x timer0_enabled: 0x%x timer0_masked: 0x%x\n", __func__,
        // status, mailbox->timer0_enabled, mailbox->timer0_masked);
        // if (mailbox->interrupts_enabled[0] & BIT(8)) {
        //     // if manual_timer unmasked, possible workaround for sepfw 18.5 exception.c:69, but it changes the timer0
        //     [sic] behavior massively
        //     // this approach breaks 14beta5
        //     return false;
        // } else
        if ((mailbox->timer0_enabled & REG_KIC_TMR_EN_MASK) == REG_KIC_TMR_EN_MASK
            && (mailbox->timer0_masked & REG_KIC_TMR_INT_MASK_MASK) == 0)
        {
            return true;
        }
    }
    else if (status == IRQ_SEP_TIMER1) {
        // if (mailbox->interrupts_enabled[0] & BIT(8)) {
        //     // if manual_timer unmasked, possible workaround for sepfw 18.5 exception.c:69, but it changes the timer0
        //     [sic] behavior massively
        //     // also doing it for timer1 just to make sure
        //     // this approach breaks 14beta5
        //     return false;
        // } else
        if ((mailbox->timer1_enabled & REG_KIC_TMR_EN_MASK) == REG_KIC_TMR_EN_MASK
            && (mailbox->timer1_masked & REG_KIC_TMR_INT_MASK_MASK) == 0)
        {
            return true;
        }
    }
    else {
        return true;
    }
    return false;
}

static bool apple_mbox_interrupt_status_empty(AppleA7IOPMailbox* mailbox)
{
    AppleA7IOPInterruptStatusMessage* message;

    QTAILQ_FOREACH (message, &mailbox->interrupt_status, entry) {
        if (is_interrupt_enabled(mailbox, message->status)) { return false; }
    }

    return true;
}

static inline bool iop_empty_is_unmasked(uint32_t int_mask) { return (int_mask & IOP_EMPTY) == 0; }

static inline bool iop_nonempty_is_unmasked(uint32_t int_mask) { return (int_mask & IOP_NONEMPTY) == 0; }

static inline bool ap_empty_is_unmasked(uint32_t int_mask) { return (int_mask & AP_EMPTY) == 0; }

static inline bool ap_nonempty_is_unmasked(uint32_t int_mask) { return (int_mask & AP_NONEMPTY) == 0; }

void apple_a7iop_mailbox_update_irq_status(AppleA7IOPMailbox* mailbox)
{
    bool iop_empty;
    bool ap_empty;
    bool iop_underflow;
    bool ap_underflow;
    bool iop_nonempty_unmasked;
    bool iop_empty_unmasked;
    bool ap_nonempty_unmasked;
    bool ap_empty_unmasked;

    iop_empty             = mailbox->iop_mailbox->inbox_count == 0;
    ap_empty              = mailbox->ap_mailbox->inbox_count == 0;
    iop_underflow         = mailbox->iop_mailbox->underflow;
    ap_underflow          = mailbox->ap_mailbox->underflow;
    iop_nonempty_unmasked = iop_nonempty_is_unmasked(mailbox->int_mask);
    iop_empty_unmasked    = iop_empty_is_unmasked(mailbox->int_mask);
    ap_nonempty_unmasked  = ap_nonempty_is_unmasked(mailbox->int_mask);
    ap_empty_unmasked     = ap_empty_is_unmasked(mailbox->int_mask);

    trace_apple_a7iop_mailbox_update_irq(mailbox->role, iop_empty, ap_empty, !iop_nonempty_unmasked,
                                         !iop_empty_unmasked, !ap_nonempty_unmasked, !ap_empty_unmasked);

    // SEP:
    // 0x65: inbox/IOP overflow
    // 0x66: inbox/IOP underflow
    // 0x67: outbox/AP overflow
    // 0x68: outbox/AP underflow

    qemu_set_irq(mailbox->irqs[APPLE_A7IOP_IRQ_IOP_NONEMPTY], (iop_nonempty_unmasked && !iop_empty) || iop_underflow);
    qemu_set_irq(mailbox->irqs[APPLE_A7IOP_IRQ_IOP_EMPTY], iop_empty_unmasked && iop_empty);

    qemu_set_irq(mailbox->irqs[APPLE_A7IOP_IRQ_AP_NONEMPTY], (ap_nonempty_unmasked && !ap_empty) || ap_underflow);
    qemu_set_irq(mailbox->irqs[APPLE_A7IOP_IRQ_AP_EMPTY], ap_empty_unmasked && ap_empty);

    mailbox->iop_nonempty = (iop_nonempty_unmasked && !iop_empty) || iop_underflow;
    mailbox->iop_empty    = iop_empty_unmasked && iop_empty;
    mailbox->ap_nonempty  = (ap_nonempty_unmasked && !ap_empty) || ap_underflow;
    mailbox->ap_empty     = ap_empty_unmasked && ap_empty;
}

void apple_a7iop_mailbox_update_irq(AppleA7IOPMailbox* mailbox)
{
    apple_a7iop_mailbox_update_irq_status(mailbox);

    bool sep_cpu_irq_raised  = mailbox->iop_nonempty || mailbox->iop_empty || mailbox->ap_nonempty || mailbox->ap_empty;
    sep_cpu_irq_raised      |= !apple_mbox_interrupt_status_empty(mailbox);
    if (!strcmp(mailbox->role, "SEP-iop")) { qemu_set_irq(mailbox->sep_cpu_irq, sep_cpu_irq_raised); }
    smp_mb();
    if (!strcmp(mailbox->role, "SEP-ap")) { apple_a7iop_mailbox_update_irq(mailbox->iop_mailbox); }
}

bool apple_a7iop_mailbox_is_empty(AppleA7IOPMailbox* mailbox)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    return mailbox->underflow || mailbox->inbox_count == 0;
}

static bool apple_a7iop_mailbox_push(AppleA7IOPMailbox* mailbox, const AppleA7IOPMessage* messages, const uint8_t count)
{
    uint8_t done = 0, i;

    assert(count <= A7IOP_MAX_MESSAGES);

    QEMU_LOCK_GUARD(&mailbox->lock);

    if (mailbox->overflow) { return false; }

    const uint8_t free = A7IOP_MAX_MESSAGES - mailbox->inbox_count;
    if (free < count) {
        mailbox->overflow = true;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s overflowed.\n", __FUNCTION__, mailbox->role);
        apple_a7iop_mailbox_update_irq(mailbox);
        return false;
    }

    while (done < count) {
        const uint8_t tail    = (mailbox->inbox_head + mailbox->inbox_count) % A7IOP_MAX_MESSAGES;
        const uint8_t pushing = MIN(count - done, MIN(free, A7IOP_MAX_MESSAGES - tail));
        for (i = 0; i < pushing; ++i) {
            mailbox->inbox[tail + i] = messages[done + i];
            trace_apple_a7iop_mailbox_send(mailbox->role, ldq_le_p(messages[done + i].data),
                                           ldq_le_p(messages[done + i].data + sizeof(uint64_t)));
        }
        mailbox->inbox_count += pushing;
        done                 += pushing;
    }

    apple_a7iop_mailbox_update_irq(mailbox);

    if (mailbox->handle_messages_bh != NULL) { qemu_bh_schedule(mailbox->handle_messages_bh); }

    return true;
}

bool apple_a7iop_mailbox_send_ap(AppleA7IOPMailbox* mailbox, const AppleA7IOPMessage* message)
{
    WITH_QEMU_LOCK_GUARD(&mailbox->lock)
    {
        if (!mailbox->ap_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n", __FUNCTION__, mailbox->role);
            return false;
        }
    }

    return apple_a7iop_mailbox_push(mailbox->ap_mailbox, message, 1);
}

bool apple_a7iop_mailbox_send_iop(AppleA7IOPMailbox* mailbox, const AppleA7IOPMessage* message)
{
    WITH_QEMU_LOCK_GUARD(&mailbox->lock)
    {
        if (!mailbox->iop_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n", __FUNCTION__, mailbox->role);
            return false;
        }
    }

    return apple_a7iop_mailbox_push(mailbox->iop_mailbox, message, 1);
}

bool apple_a7iop_inbox_peek(AppleA7IOPMailbox* mailbox, AppleA7IOPMessage* message)
{
    if (mailbox->inbox_count == 0) { return false; }
    if (message) { *message = mailbox->inbox[mailbox->inbox_head]; }
    return true;
}

static bool apple_a7iop_mailbox_pop(AppleA7IOPMailbox* mailbox, AppleA7IOPMessage* messages, const uint8_t count)
{
    uint8_t done = 0, i;

    assert(count <= A7IOP_MAX_MESSAGES);

    QEMU_LOCK_GUARD(&mailbox->lock);

    if (mailbox->underflow) { return false; }

    if (mailbox->inbox_count < count) {
        mailbox->underflow = true;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s underflowed.\n", __FUNCTION__, mailbox->role);
        apple_a7iop_mailbox_update_irq(mailbox);
        return false;
    }

    while (done < count) {
        const uint8_t head    = mailbox->inbox_head;
        const uint8_t popping = MIN(count - done, A7IOP_MAX_MESSAGES - head);
        for (i = 0; i < popping; ++i) {
            messages[done + i] = mailbox->inbox[head + i];
            stl_le_p(messages[done + i].data + 0xC, CTRL_COUNT(mailbox->inbox_count + done + i));
            trace_apple_a7iop_mailbox_recv(mailbox->role, ldq_le_p(messages[done + i].data),
                                           ldq_le_p(messages[done + i].data + sizeof(uint64_t)));
        }
        done                += popping;
        mailbox->inbox_head  = (head + popping) % A7IOP_MAX_MESSAGES;
    }

    mailbox->inbox_count -= count;
    apple_a7iop_mailbox_update_irq(mailbox);

    return true;
}

bool apple_a7iop_mailbox_recv_iop(AppleA7IOPMailbox* mailbox, AppleA7IOPMessage* message)
{
    WITH_QEMU_LOCK_GUARD(&mailbox->lock)
    {
        if (!mailbox->iop_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n", __FUNCTION__, mailbox->role);
            return false;
        }
    }

    return apple_a7iop_mailbox_pop(mailbox->iop_mailbox, message, 1);
}

bool apple_a7iop_mailbox_recv_ap(AppleA7IOPMailbox* mailbox, AppleA7IOPMessage* message)
{
    WITH_QEMU_LOCK_GUARD(&mailbox->lock)
    {
        if (!mailbox->ap_dir_en) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s direction not enabled.\n", __FUNCTION__, mailbox->role);
            return false;
        }
    }

    return apple_a7iop_mailbox_pop(mailbox->ap_mailbox, message, 1);
}

uint32_t apple_a7iop_mailbox_get_int_mask(AppleA7IOPMailbox* mailbox)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    return mailbox->int_mask;
}

void apple_a7iop_mailbox_set_int_mask(AppleA7IOPMailbox* mailbox, uint32_t value)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    mailbox->int_mask |= value;
    apple_a7iop_mailbox_update_irq(mailbox);
}

void apple_a7iop_mailbox_clear_int_mask(AppleA7IOPMailbox* mailbox, uint32_t value)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    mailbox->int_mask &= ~value;
    apple_a7iop_mailbox_update_irq(mailbox);
}

static inline uint32_t apple_a7iop_mailbox_ctrl(AppleA7IOPMailbox* mailbox)
{
    if (mailbox->underflow) { return CTRL_UNDERFLOW; }
    if (mailbox->overflow) { return CTRL_OVERFLOW; }

    return (mailbox->inbox_count >= A7IOP_MAX_MESSAGES ? CTRL_FULL : 0) | (mailbox->inbox_count == 0 ? CTRL_EMPTY : 0)
           | CTRL_COUNT(mailbox->inbox_count);
}

uint32_t apple_a7iop_mailbox_get_iop_ctrl(AppleA7IOPMailbox* mailbox)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    return (mailbox->iop_dir_en ? CTRL_ENABLE : 0) | apple_a7iop_mailbox_ctrl(mailbox->iop_mailbox);
}

void apple_a7iop_mailbox_set_iop_ctrl(AppleA7IOPMailbox* mailbox, uint32_t value)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    mailbox->iop_dir_en = (value & CTRL_ENABLE) != 0;
}

uint32_t apple_a7iop_mailbox_get_ap_ctrl(AppleA7IOPMailbox* mailbox)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    return (mailbox->ap_dir_en ? CTRL_ENABLE : 0) | apple_a7iop_mailbox_ctrl(mailbox->ap_mailbox);
}

void apple_a7iop_mailbox_set_ap_ctrl(AppleA7IOPMailbox* mailbox, uint32_t value)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    mailbox->ap_dir_en = (value & CTRL_ENABLE) != 0;
}

void apple_a7iop_interrupt_status_push(AppleA7IOPMailbox* mailbox, uint32_t status)
{
    AppleA7IOPInterruptStatusMessage* message;

#if 1
    QTAILQ_FOREACH (message, &mailbox->interrupt_status, entry) {
        if (message->status == status) {
            apple_a7iop_mailbox_update_irq(mailbox);
            return;
        }
    }
#endif

    // DON'T TEST FOR interrupts_enabled DURING PUSH!!
    // and maybe don't push when the status is already in the list
    message         = g_new0(struct AppleA7IOPInterruptStatusMessage, 1);
    message->status = status;
    QTAILQ_INSERT_TAIL(&mailbox->interrupt_status, message, entry);
    apple_a7iop_mailbox_update_irq(mailbox);
}

static void apple_a7iop_interrupt_status_remove(AppleA7IOPMailbox* mailbox, uint32_t status)
{
    AppleA7IOPInterruptStatusMessage *message, *next;

    QTAILQ_FOREACH_SAFE (message, &mailbox->interrupt_status, entry, next) {
        if (message->status == status) {
            QTAILQ_REMOVE(&mailbox->interrupt_status, message, entry);
            g_free(message);
        }
    }
}

static bool apple_a7iop_sep_timer_asserted(AppleA7IOPMailbox* mailbox, uint32_t status)
{
    if (status == IRQ_SEP_TIMER0) { return (mailbox->sep_timer_level & BIT(0)) != 0; }
    if (status == IRQ_SEP_TIMER1) { return (mailbox->sep_timer_level & BIT(1)) != 0; }
    return false;
}

uint32_t apple_a7iop_interrupt_status_pop(AppleA7IOPMailbox* mailbox)
{
    uint32_t                          ret = 0;
    AppleA7IOPInterruptStatusMessage *message, *preferred_message = NULL;
    uint32_t                          interrupt_group_message = 0, interrupt_group_preferred_message = 0;

    // order should be 0x4, 0x7, 0x1, but 0x4 shouldn't be handled here at all.

    QTAILQ_FOREACH (message, &mailbox->interrupt_status, entry) {
        interrupt_group_message = (message->status >> 16) & 0x7;
        if (is_interrupt_enabled(mailbox, message->status)) {
            if (preferred_message == NULL
                || (interrupt_group_message == interrupt_group_preferred_message
                    && message->status < preferred_message->status)
                || (interrupt_group_message != interrupt_group_preferred_message && interrupt_group_message == 0x7))
            {
                preferred_message                 = message;
                interrupt_group_preferred_message = interrupt_group_message;
            }
        }
    }

    if (preferred_message) {
        QTAILQ_REMOVE(&mailbox->interrupt_status, preferred_message, entry);
        ret = preferred_message->status;
        /*
         * The generic timer output is a level. Consuming the status does not
         * make the source stop asserting, so put it back and let the caller's
         * masking of the source decide when it becomes deliverable again. It
         * is dropped for real when the line goes low.
         */
        if (apple_a7iop_sep_timer_asserted(mailbox, ret)) {
            QTAILQ_INSERT_TAIL(&mailbox->interrupt_status, preferred_message, entry);
        }
        else {
            g_free(preferred_message);
        }
    }

    apple_a7iop_mailbox_update_irq(mailbox);

    return ret;
}

uint32_t apple_a7iop_mailbox_read_interrupt_status(AppleA7IOPMailbox* mailbox)
{
    QEMU_LOCK_GUARD(&mailbox->lock);
    // the order should be: 0x4..., 0x7..., 0x1...
    AppleA7IOPMailbox* a7iop_mbox = mailbox->iop_mailbox;
    uint32_t           interrupt_status;
    if (a7iop_mbox->iop_nonempty) {
        interrupt_status      = IRQ_IOP_NONEMPTY;
        a7iop_mbox->int_mask |= IOP_NONEMPTY;
    }
    else if (a7iop_mbox->iop_empty) {
        interrupt_status      = IRQ_IOP_EMPTY;
        a7iop_mbox->int_mask |= IOP_EMPTY;
    }
    else if (a7iop_mbox->ap_nonempty) {
        interrupt_status      = IRQ_AP_NONEMPTY;
        a7iop_mbox->int_mask |= AP_NONEMPTY;
    }
    else if (a7iop_mbox->ap_empty) {
        interrupt_status      = IRQ_AP_EMPTY;
        a7iop_mbox->int_mask |= AP_EMPTY;
    }
    else if ((interrupt_status = apple_a7iop_interrupt_status_pop(mailbox)) != 0) {
        if ((interrupt_status & 0xf0000) == 0x10000) {
            uint32_t interrupt       = interrupt_status & 0x7F;
            int      interrupt_group = interrupt / 32;
            // assert_cmpuint(interrupt_group, <, 4);
            a7iop_mbox->interrupts_enabled[interrupt_group] &= ~(BIT(interrupt % 32));
        }
        else if (interrupt_status == IRQ_IOP_NONEMPTY) {
            a7iop_mbox->int_mask |= IOP_NONEMPTY;
        }
        else if (interrupt_status == IRQ_IOP_EMPTY) {
            a7iop_mbox->int_mask |= IOP_EMPTY;
        }
        else if (interrupt_status == IRQ_AP_NONEMPTY) {
            a7iop_mbox->int_mask |= AP_NONEMPTY;
        }
        else if (interrupt_status == IRQ_AP_EMPTY) {
            a7iop_mbox->int_mask |= AP_EMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN0_NONEMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN0_NONEMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN0_EMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN0_EMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN1_NONEMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN1_NONEMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN1_EMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN1_EMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN2_NONEMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN2_NONEMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN2_EMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN2_EMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN3_NONEMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN3_NONEMPTY;
        }
        else if (interrupt_status == IRQ_MAILBOX_UNKN3_EMPTY) {
            a7iop_mbox->int_mask |= MAILBOX_MASKBIT_UNKN3_EMPTY;
        }
        else if (interrupt_status == IRQ_SEP_TIMER0) {
            a7iop_mbox->timer0_masked |= REG_KIC_TMR_INT_MASK_MASK;
        }
        else if (interrupt_status == IRQ_SEP_TIMER1) {
            a7iop_mbox->timer1_masked |= REG_KIC_TMR_INT_MASK_MASK;
        }
    }
    apple_a7iop_mailbox_update_irq(mailbox);
    return interrupt_status;
}

static void apple_a7iop_gpio_timer(AppleA7IOPMailbox* mailbox, uint32_t status, uint32_t bit, int level)
{
    QEMU_LOCK_GUARD(&mailbox->lock);

    if (level) {
        mailbox->sep_timer_level |= bit;
        // DON'T also do the checks here, only do them in interrupt_status_pop
        apple_a7iop_interrupt_status_push(mailbox, status);
    }
    else {
        mailbox->sep_timer_level &= ~bit;
        apple_a7iop_interrupt_status_remove(mailbox, status);
        apple_a7iop_mailbox_update_irq(mailbox);
    }
}

static void apple_a7iop_gpio_timer0(void* opaque, int n, int level)
{
    assert(n == 0);
    apple_a7iop_gpio_timer(opaque, IRQ_SEP_TIMER0, BIT(0), level);
}

static void apple_a7iop_gpio_timer1(void* opaque, int n, int level)
{
    assert(n == 0);
    apple_a7iop_gpio_timer(opaque, IRQ_SEP_TIMER1, BIT(1), level);
}

AppleA7IOPMailbox* apple_a7iop_mailbox_new(const char* role, AppleA7IOPVersion version, AppleA7IOPMailbox* iop_mailbox,
                                           AppleA7IOPMailbox* ap_mailbox, void* opaque,
                                           QEMUBHFunc* handle_messages_func)
{
    DeviceState*       dev;
    SysBusDevice*      sbd;
    AppleA7IOPMailbox* mailbox;
    int                i;
    char               name[128] = {0};

    dev     = qdev_new(TYPE_APPLE_A7IOP_MAILBOX);
    sbd     = SYS_BUS_DEVICE(dev);
    mailbox = APPLE_A7IOP_MAILBOX(dev);

    mailbox->role        = g_strdup(role);
    mailbox->iop_mailbox = iop_mailbox ? iop_mailbox : mailbox;
    mailbox->ap_mailbox  = ap_mailbox ? ap_mailbox : mailbox;
    if (handle_messages_func != NULL) {
        mailbox->handle_messages_bh = aio_bh_new(qemu_get_aio_context(), handle_messages_func, opaque);
    }
    QTAILQ_INIT(&mailbox->interrupt_status);
    qemu_mutex_init(&mailbox->lock);
    for (i = 0; i < APPLE_A7IOP_IRQ_MAX; i++) { sysbus_init_irq(sbd, mailbox->irqs + i); }
    qdev_init_gpio_out_named(dev, &mailbox->sep_cpu_irq, APPLE_A7IOP_SEP_CPU_IRQ, 1);

    snprintf(name, sizeof(name), TYPE_APPLE_A7IOP_MAILBOX ".%s.regs", mailbox->role);

    switch (version) {
        case APPLE_A7IOP_V2: apple_a7iop_mailbox_init_mmio_v2(mailbox, name); break;
        case APPLE_A7IOP_V4: apple_a7iop_mailbox_init_mmio_v4(mailbox, name); break;
    }

    sysbus_init_mmio(sbd, &mailbox->mmio);

    if (!strcmp(mailbox->role, "SEP-iop")) {
        qdev_init_gpio_in_named(DEVICE(mailbox), apple_a7iop_gpio_timer0, APPLE_A7IOP_SEP_GPIO_TIMER0, 1);
        qdev_init_gpio_in_named(DEVICE(mailbox), apple_a7iop_gpio_timer1, APPLE_A7IOP_SEP_GPIO_TIMER1, 1);
    }

    return mailbox;
}

static void apple_a7iop_mailbox_reset_enter(Object* obj, ResetType type)
{
    AppleA7IOPMailbox*                mailbox = APPLE_A7IOP_MAILBOX(obj);
    AppleA7IOPInterruptStatusMessage* istatus_message;
    AppleA7IOPInterruptStatusMessage* istatus_message_next;
    int                               i;

    QEMU_LOCK_GUARD(&mailbox->lock);

    assert_false(mailbox->iop_mailbox == mailbox->ap_mailbox);

    mailbox->inbox_head  = 0;
    mailbox->inbox_count = 0;
    mailbox->iop_dir_en  = true;
    mailbox->ap_dir_en   = true;
    mailbox->underflow   = false;
    memset(&mailbox->iop_recv_reg, 0, sizeof(mailbox->iop_recv_reg));
    memset(&mailbox->ap_recv_reg, 0, sizeof(mailbox->ap_recv_reg));
    memset(&mailbox->iop_send_reg, 0, sizeof(mailbox->iop_send_reg));
    memset(&mailbox->ap_send_reg, 0, sizeof(mailbox->ap_send_reg));

    QTAILQ_FOREACH_SAFE (istatus_message, &mailbox->interrupt_status, entry, istatus_message_next) {
        QTAILQ_REMOVE(&mailbox->interrupt_status, istatus_message, entry);
        g_free(istatus_message);
    }

    for (i = 0; i < ARRAY_SIZE(mailbox->interrupts_enabled); i++) { mailbox->interrupts_enabled[i] = 0; }

    mailbox->iop_nonempty   = 0;
    mailbox->iop_empty      = 0;
    mailbox->ap_nonempty    = 0;
    mailbox->ap_empty       = 0;
    mailbox->glb_cfg        = 0;
    mailbox->timer0_enabled = 0;
    mailbox->timer1_enabled = 0;
    mailbox->timer0_masked  = 0;
    mailbox->timer1_masked  = 0;
    mailbox->sepd_enabled   = 0;
}

static void apple_a7iop_mailbox_reset_hold(Object* obj, ResetType type)
{
    AppleA7IOPMailbox* mailbox = APPLE_A7IOP_MAILBOX(obj);

    apple_a7iop_mailbox_update_irq_status(mailbox);
}

static void apple_a7iop_mailbox_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_a7iop_mailbox_reset_enter;
    rc->phases.hold  = apple_a7iop_mailbox_reset_hold;

    dc->desc = "Apple A7IOP Mailbox";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleA7IOPMailbox, apple_a7iop_mailbox, APPLE_A7IOP_MAILBOX, SYS_BUS_DEVICE)
