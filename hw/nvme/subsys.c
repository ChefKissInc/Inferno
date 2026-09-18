/*
 * QEMU NVM Express Subsystem: nvme-subsys
 *
 * Copyright (c) 2021 Minwoo Im <minwoo.im.dev@gmail.com>
 *
 * This code is licensed under the GNU GPL v2.  Refer COPYING.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"

#include "nvme.h"

#define NVME_DEFAULT_RU_SIZE (96 * MiB)

int nvme_subsys_register_ctrl(NvmeCtrl* n, Error** errp)
{
    NvmeSubsystem* subsys = n->subsys;
    int            cntlid;

    for (cntlid = 0; cntlid < ARRAY_SIZE(subsys->ctrls); cntlid++) {
        if (!subsys->ctrls[cntlid]) { break; }
    }

    if (cntlid == ARRAY_SIZE(subsys->ctrls)) {
        error_setg(errp, "no more free controller id");
        return -1;
    }

    if (!subsys->serial) { subsys->serial = g_strdup(n->params.serial); }
    else if (strcmp(subsys->serial, n->params.serial)) {
        error_setg(errp, "invalid controller serial");
        return -1;
    }

    subsys->ctrls[cntlid] = n;

    return cntlid;
}

void nvme_subsys_unregister_ctrl(NvmeSubsystem* subsys, NvmeCtrl* n)
{
    subsys->ctrls[n->cntlid] = NULL;

    n->cntlid = -1;
}

static bool nvme_calc_rgif(uint16_t nruh, uint16_t nrg, uint8_t* rgif)
{
    uint16_t     val;
    unsigned int i;

    if (unlikely(nrg == 1)) {
        /* PIDRG_NORGI scenario, all of pid is used for PHID */
        *rgif = 0;
        return true;
    }

    val = nrg;
    i   = 0;
    while (val) {
        val >>= 1;
        i++;
    }
    *rgif = i;

    /* ensure remaining bits suffice to represent number of phids in a RG */
    if (unlikely((UINT16_MAX >> i) < nruh)) {
        *rgif = 0;
        return false;
    }

    return true;
}

static bool nvme_subsys_setup_fdp(NvmeSubsystem* subsys, Error** errp)
{
    NvmeEnduranceGroup* endgrp = &subsys->endgrp;

    if (!subsys->params.fdp.runs) {
        error_setg(errp, "fdp.runs must be non-zero");
        return false;
    }

    endgrp->fdp.runs = subsys->params.fdp.runs;

    if (!subsys->params.fdp.nrg) {
        error_setg(errp, "fdp.nrg must be non-zero");
        return false;
    }

    endgrp->fdp.nrg = subsys->params.fdp.nrg;

    if (!subsys->params.fdp.nruh || subsys->params.fdp.nruh > NVME_FDP_MAXPIDS) {
        error_setg(errp, "fdp.nruh must be non-zero and less than %u", NVME_FDP_MAXPIDS);
        return false;
    }

    endgrp->fdp.nruh = subsys->params.fdp.nruh;

    if (!nvme_calc_rgif(endgrp->fdp.nruh, endgrp->fdp.nrg, &endgrp->fdp.rgif)) {
        error_setg(errp, "cannot derive a valid rgif (nruh %" PRIu16 " nrg %" PRIu32 ")", endgrp->fdp.nruh,
                   endgrp->fdp.nrg);
        return false;
    }

    endgrp->fdp.ruhs = g_new(NvmeRuHandle, endgrp->fdp.nruh);

    for (uint16_t ruhid = 0; ruhid < endgrp->fdp.nruh; ruhid++) {
        endgrp->fdp.ruhs[ruhid] = (NvmeRuHandle){
            .ruht = NVME_RUHT_INITIALLY_ISOLATED,
            .ruha = NVME_RUHA_UNUSED,
        };

        endgrp->fdp.ruhs[ruhid].rus = g_new(NvmeReclaimUnit, endgrp->fdp.nrg);
    }

    endgrp->fdp.enabled = true;

    return true;
}

static bool nvme_subsys_setup(NvmeSubsystem* subsys, Error** errp)
{
    const char* nqn = subsys->params.nqn ? subsys->params.nqn : subsys->parent_obj.id;

    snprintf((char*)subsys->subnqn, sizeof(subsys->subnqn), "nqn.2019-08.org.qemu:%s", nqn);

    if (subsys->params.fdp.enabled && !nvme_subsys_setup_fdp(subsys, errp)) { return false; }

    return true;
}

static void nvme_subsys_realize(DeviceState* dev, Error** errp)
{
    NvmeSubsystem* subsys = NVME_SUBSYS(dev);

    qbus_init(&subsys->bus, sizeof(NvmeBus), TYPE_NVME_BUS, dev, dev->id);

    nvme_subsys_setup(subsys, errp);
}

static const Property nvme_subsystem_props[] = {
    DEFINE_PROP_STRING("nqn", NvmeSubsystem, params.nqn),
    DEFINE_PROP_BOOL("fdp", NvmeSubsystem, params.fdp.enabled, false),
    DEFINE_PROP_SIZE("fdp.runs", NvmeSubsystem, params.fdp.runs, NVME_DEFAULT_RU_SIZE),
    DEFINE_PROP_UINT32("fdp.nrg", NvmeSubsystem, params.fdp.nrg, 1),
    DEFINE_PROP_UINT16("fdp.nruh", NvmeSubsystem, params.fdp.nruh, 0),
};

static void nvme_subsys_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    dc->realize = nvme_subsys_realize;
    dc->desc    = "Virtual NVMe subsystem";

    device_class_set_props(dc, nvme_subsystem_props);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(NvmeSubsystem, nvme_subsys, NVME_SUBSYS, DEVICE)
