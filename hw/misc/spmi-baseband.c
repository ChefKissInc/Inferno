#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/misc/spmi-baseband.h"
#include "qemu/module.h"

// AppleBasebandI19::spmiTimerCallback says that the only thing being
// written/read is offset 0x147/value 0x2f
// commands being used are: SPMI_CMD_EXT_WRITEL/SPMI_CMD_EXT_READL

#if 0
    #define DPRINTF(v, ...) fprintf(stderr, v, ##__VA_ARGS__)
#else
    #define DPRINTF(v, ...) \
        do { }              \
        while (0)
#endif

#define RREG32(off)         ldl_le_p(&s->reg[off])
#define WREG32(off, val)    stl_le_p(&s->reg[off], val)
#define WREG32_OR(off, val) WREG32(off, RREG32(off) | val)

void apple_spmi_baseband_set_irq(AppleSPMIBasebandState* s, int value)
{
    if (value) { qemu_irq_raise(s->irq); }
    else {
        qemu_irq_lower(s->irq);
    }
}

static int apple_spmi_baseband_send(SPMISlave* s, uint8_t* data, uint8_t len)
{
    AppleSPMIBasebandState* p = APPLE_SPMI_BASEBAND(s);
    // bool aflg = false;
    uint16_t addr, end_addr;
    DPRINTF("%s: addr 0x%x len 0x%x\n", __func__, p->addr, len);

    for (addr = p->addr, end_addr = p->addr + len; addr < end_addr; addr++) { p->reg[addr] = data[addr - p->addr]; }
    if (p->addr == 0x147 && p->reg[p->addr] == 0x2f) {
        DPRINTF("%s: addr 0x%x valid\n", __func__, p->addr);
        // is this interrupt actually affecting anything?
        // apple_spmi_baseband_set_irq(p, 1);
    }
    else {
        DPRINTF("%s: addr 0x%x INVALID\n", __func__, p->addr);
    }
    p->addr = addr;
    return len;
}

static int apple_spmi_baseband_recv(SPMISlave* s, uint8_t* data, uint8_t len)
{
    AppleSPMIBasebandState* p = APPLE_SPMI_BASEBAND(s);
    uint16_t                addr, end_addr;
    DPRINTF("%s: addr 0x%x len 0x%x\n", __func__, p->addr, len);

    for (addr = p->addr, end_addr = p->addr + len; addr < end_addr; addr++) { data[addr - p->addr] = p->reg[addr]; }
    p->addr = addr;
    return len;
}

static int apple_spmi_baseband_command(SPMISlave* s, uint8_t opcode, uint16_t addr)
{
    AppleSPMIBasebandState* p = APPLE_SPMI_BASEBAND(s);
    p->addr                   = addr;
    DPRINTF("%s: opcode 0x%x addr 0x%x\n", __func__, opcode, addr);

    switch (opcode) {
        case SPMI_CMD_EXT_READ  :
        case SPMI_CMD_EXT_READL :
        case SPMI_CMD_EXT_WRITE :
        case SPMI_CMD_EXT_WRITEL: return 0;
        default                 : return 1;
    }
}

DeviceState* apple_spmi_baseband_create(AppleDTNode* node)
{
    DeviceState*            dev = qdev_new(TYPE_APPLE_SPMI_BASEBAND);
    AppleSPMIBasebandState* p   = APPLE_SPMI_BASEBAND(dev);
    AppleDTProp*            prop;

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    spmi_set_slave_sid(SPMI_SLAVE(dev), *(uint32_t*)prop->data);

    qdev_init_gpio_out(dev, &p->irq, 1);
    return dev;
}

static void apple_spmi_baseband_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*    dc = DEVICE_CLASS(klass);
    SPMISlaveClass* sc = SPMI_SLAVE_CLASS(klass);

    dc->desc = "Apple SPMI Baseband";

    sc->send    = apple_spmi_baseband_send;
    sc->recv    = apple_spmi_baseband_recv;
    sc->command = apple_spmi_baseband_command;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSPMIBasebandState, apple_spmi_baseband, APPLE_SPMI_BASEBAND, SPMI_SLAVE)
