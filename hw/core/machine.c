/*
 * QEMU Machine
 *
 * Copyright (C) 2014 Red Hat Inc
 *
 * Authors:
 *   Marcel Apfelbaum <marcel.a@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/units.h"
#include "qemu/accel.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-machine.h"
#include "qapi/qapi-commands-machine.h"
#include "qemu/madvise.h"
#include "qom/object_interfaces.h"
#include "system/cpus.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "hw/pci/pci_bridge.h"
#include "audio/audio.h"

MachineState* current_machine;

static char* machine_get_kernel(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return g_strdup(ms->kernel_filename);
}

static void machine_set_kernel(Object* obj, const char* value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    g_free(ms->kernel_filename);
    ms->kernel_filename = g_strdup(value);
}

static char* machine_get_initrd(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return g_strdup(ms->initrd_filename);
}

static void machine_set_initrd(Object* obj, const char* value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    g_free(ms->initrd_filename);
    ms->initrd_filename = g_strdup(value);
}

static char* machine_get_append(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return g_strdup(ms->kernel_cmdline);
}

static void machine_set_append(Object* obj, const char* value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    g_free(ms->kernel_cmdline);
    ms->kernel_cmdline = g_strdup(value);
}

static char* machine_get_dtb(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return g_strdup(ms->dtb);
}

static void machine_set_dtb(Object* obj, const char* value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    g_free(ms->dtb);
    ms->dtb = g_strdup(value);
}

static bool machine_get_dump_guest_core(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return ms->dump_guest_core;
}

static void machine_set_dump_guest_core(Object* obj, bool value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    if (!value && QEMU_MADV_DONTDUMP == QEMU_MADV_INVALID) {
        error_setg(errp, "Dumping guest memory cannot be disabled on this host");
        return;
    }
    ms->dump_guest_core = value;
}

static bool machine_get_mem_merge(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return ms->mem_merge;
}

static void machine_set_mem_merge(Object* obj, bool value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    if (value && QEMU_MADV_MERGEABLE == QEMU_MADV_INVALID) {
        error_setg(errp, "Memory merging is not supported on this host");
        return;
    }
    ms->mem_merge = value;
}

#ifdef CONFIG_POSIX
static bool machine_get_aux_ram_share(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return ms->aux_ram_share;
}

static void machine_set_aux_ram_share(Object* obj, bool value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    ms->aux_ram_share = value;
}
#endif

static bool machine_get_usb(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return ms->usb;
}

static void machine_set_usb(Object* obj, bool value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    ms->usb          = value;
    ms->usb_disabled = !value;
}

static bool machine_get_graphics(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return ms->enable_graphics;
}

static void machine_set_graphics(Object* obj, bool value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    ms->enable_graphics = value;
}

static char* machine_get_firmware(Object* obj, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    return g_strdup(ms->firmware);
}

static void machine_set_firmware(Object* obj, const char* value, Error** errp)
{
    MachineState* ms = MACHINE(obj);

    g_free(ms->firmware);
    ms->firmware = g_strdup(value);
}

static void machine_get_mem(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp)
{
    MachineState*           ms  = MACHINE(obj);
    MemorySizeConfiguration mem = {
        .has_size     = true,
        .size         = ms->ram_size,
        .has_max_size = !!ms->ram_slots,
        .max_size     = ms->maxram_size,
        .has_slots    = !!ms->ram_slots,
        .slots        = ms->ram_slots,
    };
    MemorySizeConfiguration* p_mem = &mem;

    visit_type_MemorySizeConfiguration(v, name, &p_mem, &error_abort);
}

static void machine_set_mem(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp)
{
    ERRP_GUARD();
    MachineState*            ms = MACHINE(obj);
    MachineClass*            mc = MACHINE_GET_CLASS(obj);
    MemorySizeConfiguration* mem;

    if (!visit_type_MemorySizeConfiguration(v, name, &mem, errp)) { return; }

    if (!mem->has_size) {
        mem->has_size = true;
        mem->size     = mc->default_ram_size;
    }
    mem->size = QEMU_ALIGN_UP(mem->size, 8192);
    if (mc->fixup_ram_size) { mem->size = mc->fixup_ram_size(mem->size); }
    if ((ram_addr_t)mem->size != mem->size) {
        error_setg(errp, "ram size %llu exceeds permitted maximum %llu", (unsigned long long)mem->size,
                   (unsigned long long)RAM_ADDR_MAX);
        goto out_free;
    }

    if (mem->has_max_size) {
        if ((ram_addr_t)mem->max_size != mem->max_size) {
            error_setg(errp, "ram size %llu exceeds permitted maximum %llu", (unsigned long long)mem->max_size,
                       (unsigned long long)RAM_ADDR_MAX);
            goto out_free;
        }
        if (mem->max_size < mem->size) {
            error_setg(errp,
                       "invalid value of maxmem: "
                       "maximum memory size (0x%" PRIx64 ") must be at least "
                       "the initial memory size (0x%" PRIx64 ")",
                       mem->max_size, mem->size);
            goto out_free;
        }
        if (mem->has_slots && mem->slots && mem->max_size == mem->size) {
            error_setg(errp,
                       "invalid value of maxmem: "
                       "memory slots were specified but maximum memory size "
                       "(0x%" PRIx64 ") is equal to the initial memory size "
                       "(0x%" PRIx64 ")",
                       mem->max_size, mem->size);
            goto out_free;
        }
        ms->maxram_size = mem->max_size;
    }
    else {
        if (mem->has_slots) {
            error_setg(errp, "slots specified but no max-size");
            goto out_free;
        }
        ms->maxram_size = mem->size;
    }
    ms->ram_size  = mem->size;
    ms->ram_slots = mem->has_slots ? mem->slots : 0;
out_free:
    qapi_free_MemorySizeConfiguration(mem);
}

void machine_class_allow_dynamic_sysbus_dev(MachineClass* mc, const char* type)
{ QAPI_LIST_PREPEND(mc->allowed_dynamic_sysbus_devices, g_strdup(type)); }

bool device_is_dynamic_sysbus(MachineClass* mc, DeviceState* dev)
{
    Object* obj = OBJECT(dev);

    if (!object_dynamic_cast(obj, TYPE_SYS_BUS_DEVICE)) { return false; }

    return device_type_is_dynamic_sysbus(mc, object_get_typename(obj));
}

bool device_type_is_dynamic_sysbus(MachineClass* mc, const char* type)
{
    bool         allowed = false;
    strList*     wl;
    ObjectClass* klass = object_class_by_name(type);

    for (wl = mc->allowed_dynamic_sysbus_devices; !allowed && wl; wl = wl->next) {
        allowed |= !!object_class_dynamic_cast(klass, wl->value);
    }

    return allowed;
}

HotpluggableCPUList* machine_query_hotpluggable_cpus(MachineState* machine)
{
    int                  i;
    HotpluggableCPUList* head = NULL;
    MachineClass*        mc   = MACHINE_GET_CLASS(machine);

    /* force board to initialize possible_cpus if it hasn't been done yet */
    mc->possible_cpu_arch_ids(machine);

    for (i = 0; i < machine->possible_cpus->len; i++) {
        CPUState*        cpu;
        HotpluggableCPU* cpu_item = g_new0(typeof(*cpu_item), 1);

        cpu_item->type        = g_strdup(machine->possible_cpus->cpus[i].type);
        cpu_item->vcpus_count = machine->possible_cpus->cpus[i].vcpus_count;
        cpu_item->props       = g_memdup(&machine->possible_cpus->cpus[i].props, sizeof(*cpu_item->props));

        cpu = machine->possible_cpus->cpus[i].cpu;
        if (cpu) { cpu_item->qom_path = object_get_canonical_path(OBJECT(cpu)); }
        QAPI_LIST_PREPEND(head, cpu_item);
    }
    return head;
}

static void machine_get_smp(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp)
{
    MachineState*     ms     = MACHINE(obj);
    SMPConfiguration* config = &(SMPConfiguration){
        .has_cpus     = true,
        .cpus         = ms->smp.cpus,
        .has_drawers  = true,
        .drawers      = ms->smp.drawers,
        .has_books    = true,
        .books        = ms->smp.books,
        .has_sockets  = true,
        .sockets      = ms->smp.sockets,
        .has_dies     = true,
        .dies         = ms->smp.dies,
        .has_clusters = true,
        .clusters     = ms->smp.clusters,
        .has_modules  = true,
        .modules      = ms->smp.modules,
        .has_cores    = true,
        .cores        = ms->smp.cores,
        .has_threads  = true,
        .threads      = ms->smp.threads,
        .has_maxcpus  = true,
        .maxcpus      = ms->smp.max_cpus,
    };

    if (!visit_type_SMPConfiguration(v, name, &config, &error_abort)) { return; }
}

static void machine_set_smp(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp)
{
    MachineState* ms                   = MACHINE(obj);
    g_autoptr(SMPConfiguration) config = NULL;

    if (!visit_type_SMPConfiguration(v, name, &config, errp)) { return; }

    machine_parse_smp_config(ms, config, errp);
}

static void machine_get_smp_cache(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp)
{
    MachineState*            ms    = MACHINE(obj);
    SmpCache*                cache = &ms->smp_cache;
    SmpCachePropertiesList*  head  = NULL;
    SmpCachePropertiesList** tail  = &head;

    for (int i = 0; i < CACHE_LEVEL_AND_TYPE__MAX; i++) {
        SmpCacheProperties* node = g_new(SmpCacheProperties, 1);

        node->cache    = cache->props[i].cache;
        node->topology = cache->props[i].topology;
        QAPI_LIST_APPEND(tail, node);
    }

    visit_type_SmpCachePropertiesList(v, name, &head, errp);
    qapi_free_SmpCachePropertiesList(head);
}

static void machine_set_smp_cache(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp)
{
    MachineState*           ms = MACHINE(obj);
    SmpCachePropertiesList* caches;

    if (!visit_type_SmpCachePropertiesList(v, name, &caches, errp)) { return; }

    machine_parse_smp_cache(ms, caches, errp);
    qapi_free_SmpCachePropertiesList(caches);
}

static bool create_default_memdev(MachineState* ms, const char* path, Error** errp)
{
    Object*       obj;
    MachineClass* mc = MACHINE_GET_CLASS(ms);
    bool          r  = false;

    obj = object_new(path ? TYPE_MEMORY_BACKEND_FILE : TYPE_MEMORY_BACKEND_RAM);
    if (path) {
        if (!object_property_set_str(obj, "mem-path", path, errp)) { goto out; }
    }
    if (!object_property_set_int(obj, "size", ms->ram_size, errp)) { goto out; }
    object_property_add_child(object_get_objects_root(), mc->default_ram_id, obj);
    /* Ensure backend's memory region name is equal to mc->default_ram_id */
    if (!object_property_set_bool(obj, "x-use-canonical-path-for-ramblock-id", false, errp)) { goto out; }
    if (!user_creatable_complete(USER_CREATABLE(obj), errp)) { goto out; }
    r = object_property_set_link(OBJECT(ms), "memory-backend", obj, errp);

out:
    object_unref(obj);
    return r;
}

static void machine_class_init(ObjectClass* oc, const void* data)
{
    MachineClass* mc = MACHINE_CLASS(oc);

    /* Default 128 MB as guest ram size */
    mc->default_ram_size = 128 * MiB;

    mc->create_default_memdev = create_default_memdev;

    object_class_property_add_str(oc, "kernel", machine_get_kernel, machine_set_kernel);
    object_class_property_set_description(oc, "kernel", "Kernel image file");

    object_class_property_add_str(oc, "initrd", machine_get_initrd, machine_set_initrd);
    object_class_property_set_description(oc, "initrd", "Initial ramdisk file");

    object_class_property_add_str(oc, "append", machine_get_append, machine_set_append);
    object_class_property_set_description(oc, "append", "Kernel command line");

    object_class_property_add_str(oc, "dtb", machine_get_dtb, machine_set_dtb);
    object_class_property_set_description(oc, "dtb", "Kernel device tree file");

    object_class_property_add(oc, "smp", "SMPConfiguration", machine_get_smp, machine_set_smp, NULL, NULL);
    object_class_property_set_description(oc, "smp", "CPU topology");

    object_class_property_add(oc, "smp-cache", "SmpCachePropertiesWrapper", machine_get_smp_cache,
                              machine_set_smp_cache, NULL, NULL);
    object_class_property_set_description(oc, "smp-cache", "Cache properties list for SMP machine");

    object_class_property_add_bool(oc, "dump-guest-core", machine_get_dump_guest_core, machine_set_dump_guest_core);
    object_class_property_set_description(oc, "dump-guest-core", "Include guest memory in a core dump");

    object_class_property_add_bool(oc, "mem-merge", machine_get_mem_merge, machine_set_mem_merge);
    object_class_property_set_description(oc, "mem-merge", "Enable/disable memory merge support");

#ifdef CONFIG_POSIX
    object_class_property_add_bool(oc, "aux-ram-share", machine_get_aux_ram_share, machine_set_aux_ram_share);
    object_class_property_set_description(oc, "aux-ram-share", "Use anonymous shared memory for auxiliary guest RAMs");
#endif

    object_class_property_add_bool(oc, "usb", machine_get_usb, machine_set_usb);
    object_class_property_set_description(oc, "usb", "Set on/off to enable/disable usb");

    object_class_property_add_bool(oc, "graphics", machine_get_graphics, machine_set_graphics);
    object_class_property_set_description(oc, "graphics", "Set on/off to enable/disable graphics emulation");

    object_class_property_add_str(oc, "firmware", machine_get_firmware, machine_set_firmware);
    object_class_property_set_description(oc, "firmware", "Firmware image");

    object_class_property_add_link(oc, "memory-backend", TYPE_MEMORY_BACKEND, offsetof(MachineState, memdev),
                                   object_property_allow_set_link, OBJ_PROP_LINK_STRONG);
    object_class_property_set_description(oc, "memory-backend",
                                          "Set RAM backend"
                                          "Valid value is ID of hostmem based backend");

    object_class_property_add(oc, "memory", "MemorySizeConfiguration", machine_get_mem, machine_set_mem, NULL, NULL);
    object_class_property_set_description(oc, "memory", "Memory size configuration");
}

static void machine_class_base_init(ObjectClass* oc, const void* data)
{
    MachineClass* mc = MACHINE_CLASS(oc);
    mc->max_cpus     = mc->max_cpus ?: 1;
    mc->min_cpus     = mc->min_cpus ?: 1;
    mc->default_cpus = mc->default_cpus ?: 1;

    if (!object_class_is_abstract(oc)) {
        const char* cname = object_class_get_name(oc);
        assert(g_str_has_suffix(cname, TYPE_MACHINE_SUFFIX));
        mc->name         = g_strndup(cname, strlen(cname) - strlen(TYPE_MACHINE_SUFFIX));
        mc->compat_props = g_ptr_array_new();
    }
}

static void machine_initfn(Object* obj)
{
    MachineState* ms = MACHINE(obj);
    MachineClass* mc = MACHINE_GET_CLASS(obj);

    ms->dump_guest_core = true;
    ms->mem_merge       = (QEMU_MADV_MERGEABLE != QEMU_MADV_INVALID);
    ms->enable_graphics = true;
    ms->kernel_cmdline  = g_strdup("");
    ms->ram_size        = mc->default_ram_size;
    ms->maxram_size     = mc->default_ram_size;

    /* default to mc->default_cpus */
    ms->smp.cpus     = mc->default_cpus;
    ms->smp.max_cpus = mc->default_cpus;
    ms->smp.drawers  = 1;
    ms->smp.books    = 1;
    ms->smp.sockets  = 1;
    ms->smp.dies     = 1;
    ms->smp.clusters = 1;
    ms->smp.modules  = 1;
    ms->smp.cores    = 1;
    ms->smp.threads  = 1;

    for (int i = 0; i < CACHE_LEVEL_AND_TYPE__MAX; i++) {
        ms->smp_cache.props[i].cache    = (CacheLevelAndType)i;
        ms->smp_cache.props[i].topology = CPU_TOPOLOGY_LEVEL_DEFAULT;
    }
}

static void machine_finalize(Object* obj)
{
    MachineState* ms = MACHINE(obj);

    g_free(ms->kernel_filename);
    g_free(ms->initrd_filename);
    g_free(ms->kernel_cmdline);
    g_free(ms->dtb);
    g_free(ms->firmware);
    g_free(ms->device_memory);
}

bool machine_usb(MachineState* machine) { return machine->usb; }

bool machine_dump_guest_core(MachineState* machine) { return machine->dump_guest_core; }

bool machine_mem_merge(MachineState* machine) { return machine->mem_merge; }

MemoryRegion* machine_consume_memdev(MachineState* machine, HostMemoryBackend* backend)
{
    MemoryRegion* ret = host_memory_backend_get_memory(backend);

    if (host_memory_backend_is_mapped(backend)) {
        error_report("memory backend %s can't be used multiple times.",
                     object_get_canonical_path_component(OBJECT(backend)));
        exit(EXIT_FAILURE);
    }
    host_memory_backend_set_mapped(backend, true);
    return ret;
}

const char* machine_class_default_cpu_type(MachineClass* mc)
{
    if (mc->valid_cpu_types && !mc->valid_cpu_types[1]) {
        /* Only a single CPU type allowed: use it as default. */
        return mc->valid_cpu_types[0];
    }
    return mc->default_cpu_type;
}

static bool is_cpu_type_supported(const MachineState* machine, Error** errp)
{
    MachineClass* mc = MACHINE_GET_CLASS(machine);
    ObjectClass*  oc = object_class_by_name(machine->cpu_type);
    CPUClass*     cc;
    int           i;

    /*
     * Check if the user specified CPU type is supported when the valid
     * CPU types have been determined. Note that the user specified CPU
     * type is provided through '-cpu' option.
     */
    if (mc->valid_cpu_types) {
        assert(mc->valid_cpu_types[0] != NULL);
        for (i = 0; mc->valid_cpu_types[i]; i++) {
            if (object_class_dynamic_cast(oc, mc->valid_cpu_types[i])) { break; }
        }

        /* The user specified CPU type isn't valid */
        if (!mc->valid_cpu_types[i]) {
            g_autofree char* requested = cpu_model_from_type(machine->cpu_type);
            error_setg(errp, "Invalid CPU model: %s", requested);
            if (!mc->valid_cpu_types[1]) {
                g_autofree char* model = cpu_model_from_type(mc->valid_cpu_types[0]);
                error_append_hint(errp, "The only valid type is: %s\n", model);
            }
            else {
                error_append_hint(errp, "The valid models are: ");
                for (i = 0; mc->valid_cpu_types[i]; i++) {
                    g_autofree char* model = cpu_model_from_type(mc->valid_cpu_types[i]);
                    error_append_hint(errp, "%s%s", model, mc->valid_cpu_types[i + 1] ? ", " : "");
                }
                error_append_hint(errp, "\n");
            }

            return false;
        }
    }

    /* Check if CPU type is deprecated and warn if so */
    cc = CPU_CLASS(oc);
    assert(cc != NULL);
    if (cc->deprecation_note) {
        warn_report("CPU model %s is deprecated -- %s", machine->cpu_type, cc->deprecation_note);
    }

    return true;
}

void machine_run_board_init(MachineState* machine, const char* mem_path, Error** errp)
{
    ERRP_GUARD();
    MachineClass* machine_class = MACHINE_GET_CLASS(machine);

    /* On 32-bit hosts, QEMU is limited by virtual address space */
    if (machine->ram_size > (2047 << 20) && HOST_LONG_BITS == 32) {
        error_setg(errp, "at most 2047 MB RAM can be simulated");
        return;
    }

    if (machine->memdev) {
        ram_addr_t backend_size = object_property_get_uint(OBJECT(machine->memdev), "size", &error_abort);
        if (backend_size != machine->ram_size) {
            error_setg(errp, "Machine memory size does not match the size of the memory backend");
            return;
        }
    }
    else if (machine_class->default_ram_id && machine->ram_size) {
        if (object_property_find(object_get_objects_root(), machine_class->default_ram_id)) {
            error_setg(errp,
                       "object's id '%s' is reserved for the default"
                       " RAM backend, it can't be used for any other purposes",
                       machine_class->default_ram_id);
            error_append_hint(errp,
                              "Change the object's 'id' to something else or disable"
                              " automatic creation of the default RAM backend by setting"
                              " 'memory-backend=%s' with '-machine'.\n",
                              machine_class->default_ram_id);
            return;
        }

        if (!machine_class->create_default_memdev(current_machine, mem_path, errp)) { return; }
    }

    if (!machine->ram && machine->memdev) { machine->ram = machine_consume_memdev(machine, machine->memdev); }

    /* Check if the CPU type is supported */
    if (machine->cpu_type && !is_cpu_type_supported(machine, errp)) { return; }

    accel_init_interfaces(ACCEL_GET_CLASS(machine->accelerator));
    machine_class->init(machine);
    phase_advance(PHASE_MACHINE_INITIALIZED);
}

static NotifierList machine_init_done_notifiers = NOTIFIER_LIST_INITIALIZER(machine_init_done_notifiers);

void qemu_add_machine_init_done_notifier(Notifier* notify)
{
    notifier_list_add(&machine_init_done_notifiers, notify);
    if (phase_check(PHASE_MACHINE_READY)) { notify->notify(notify, NULL); }
}

void qemu_remove_machine_init_done_notifier(Notifier* notify) { notifier_remove(notify); }

void qdev_machine_creation_done(void)
{
    cpu_synchronize_all_post_init();

    /*
     * ok, initial machine setup is done, starting from now we can
     * only create hotpluggable devices
     */
    phase_advance(PHASE_MACHINE_READY);
    qdev_assert_realized_properly();

    /* TODO: once all bus devices are qdevified, this should be done
     * when bus is created by qdev.c */
    /*
     * This is where we arrange for the sysbus to be reset when the
     * whole simulation is reset. In turn, resetting the sysbus will cause
     * all devices hanging off it (and all their child buses, recursively)
     * to be reset. Note that this will *not* reset any Device objects
     * which are not attached to some part of the qbus tree!
     */
    qemu_register_resettable(OBJECT(sysbus_get_default()));

    notifier_list_notify(&machine_init_done_notifiers, NULL);

    qemu_system_reset(SHUTDOWN_CAUSE_NONE);
}

static const TypeInfo machine_info = {
    .name            = TYPE_MACHINE,
    .parent          = TYPE_OBJECT,
    .abstract        = true,
    .class_size      = sizeof(MachineClass),
    .class_init      = machine_class_init,
    .class_base_init = machine_class_base_init,
    OBJECT_TYPE_INSTANCE(MachineState),
    .instance_init     = machine_initfn,
    .instance_finalize = machine_finalize,
};

DEFINE_TYPE(machine_info)
