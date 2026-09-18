/*
 * Internal execution defines for qemu (target agnostic)
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "exec/cpu-common.h"
#include "exec/translation-block.h"
#include "accel/tcg/tb-cpu-state.h"

extern bool one_insn_per_tb;

/*
 * Return true if CS is not running in parallel with other cpus, either
 * because there are no other cpus or we are within an exclusive context.
 */
static inline bool cpu_in_serial_context(CPUState* cs)
{ return !tcg_cflags_has(cs, CF_PARALLEL) || cpu_in_exclusive_context(cs); }

TranslationBlock* tb_gen_code(CPUState* cpu, TCGTBCPUState s);
void              page_init(void);
void              tb_htable_init(void);
void              tb_reset_jump(TranslationBlock* tb, int n);
TranslationBlock* tb_link_page(TranslationBlock* tb);
void              cpu_restore_state_from_tb(CPUState* cpu, TranslationBlock* tb, uintptr_t host_pc);

/**
 * tlb_init - initialize a CPU's TLB
 * @cpu: CPU whose TLB should be initialized
 */
void tlb_init(CPUState* cpu);
/**
 * tlb_destroy - destroy a CPU's TLB
 * @cpu: CPU whose TLB should be destroyed
 */
void tlb_destroy(CPUState* cpu);

bool tcg_exec_realizefn(CPUState* cpu, Error** errp);
void tcg_exec_unrealizefn(CPUState* cpu);

/* current cflags for hashing/comparison */
uint32_t curr_cflags(CPUState* cpu);

void tb_check_watchpoint(CPUState* cpu, uintptr_t retaddr);

/**
 * get_page_addr_code_hostp()
 * @env: CPUArchState
 * @addr: guest virtual address of guest code
 *
 * See get_page_addr_code() (full-system version) for documentation on the
 * return value.
 *
 * Sets *@hostp as follows.
 * If the return value is -1, sets *@hostp to NULL. Otherwise, sets *@hostp
 * to the host address where @addr's content is kept.
 *
 * Note: this function can trigger an exception.
 */
tb_page_addr_t get_page_addr_code_hostp(CPUArchState* env, vaddr addr, void** hostp);

/**
 * get_page_addr_code()
 * @env: CPUArchState
 * @addr: guest virtual address of guest code
 *
 * If we cannot translate and execute from the entire RAM page, or if
 * the region is not backed by RAM, returns -1. Otherwise, returns the
 * ram_addr_t corresponding to the guest code at @addr.
 *
 * Note: this function can trigger an exception.
 */
static inline tb_page_addr_t get_page_addr_code(CPUArchState* env, vaddr addr)
{
    void* discard;
    return get_page_addr_code_hostp(env, addr, &discard);
}

#if defined(CONFIG_SOFTMMU) && defined(CONFIG_DEBUG_TCG)
void assert_no_pages_locked(void);
#else
static inline void assert_no_pages_locked(void) { }
#endif

void page_table_config_init(void);

G_NORETURN void cpu_io_recompile(CPUState* cpu, uintptr_t retaddr);

void tb_phys_invalidate(TranslationBlock* tb, tb_page_addr_t page_addr);
void tb_set_jmp_target(TranslationBlock* tb, int n, uintptr_t addr);

void tcg_get_stats(AccelState* accel, GString* buf);
