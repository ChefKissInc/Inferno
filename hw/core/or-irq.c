/*
 * QEMU IRQ/GPIO common code.
 *
 * Copyright (c) 2016 Alistair Francis <alistair@alistair23.me>.
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
#include "hw/irq.h"
#include "hw/or-irq.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"

static void or_irq_handler(void* opaque, int n, int level)
{
    OrIRQState* s        = opaque;
    int         or_level = 0;
    int         i;

    s->levels[n] = level;

    for (i = 0; i < s->num_lines; i++) { or_level |= s->levels[i]; }

    qemu_set_irq(s->out_irq, or_level);
}

static void or_irq_reset_enter(Object* obj, ResetType type)
{
    OrIRQState* s = OR_IRQ(obj);
    int         i;

    for (i = 0; i < MAX_OR_LINES; i++) { s->levels[i] = false; }
}

static void or_irq_realize(DeviceState* dev, Error** errp)
{
    OrIRQState* s = OR_IRQ(dev);

    assert(s->num_lines <= MAX_OR_LINES);

    qdev_init_gpio_in(dev, or_irq_handler, s->num_lines);
}

static void or_irq_init(Object* obj)
{
    OrIRQState* s = OR_IRQ(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->out_irq, 1);
}

static const Property or_irq_properties[] = {
    DEFINE_PROP_UINT16("num-lines", OrIRQState, num_lines, 1),
};

static void or_irq_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = or_irq_reset_enter;

    device_class_set_props(dc, or_irq_properties);
    dc->realize        = or_irq_realize;
    dc->user_creatable = false;
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(OrIRQState, or_irq, OR_IRQ, DEVICE)
