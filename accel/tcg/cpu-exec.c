/*
 *  emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "hw/core/cpu.h"
#include "accel/tcg/cpu-ops.h"
#include "trace.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "qemu/atomic.h"
#include "qemu/rcu.h"
#include "exec/log.h"
#include "qemu/main-loop.h"
#include "system/tcg.h"
#include "exec/helper-proto-common.h"
#include "tcg-accel-ops.h"
#include "tb-jmp-cache.h"
#include "tb-hash.h"
#include "tb-context.h"
#include "tb-internal.h"
#include "internal-common.h"

struct tb_desc
{
    TCGTBCPUState  s;
    CPUArchState*  env;
    tb_page_addr_t page_addr0;
};

static bool tb_lookup_cmp(const void* p, const void* d)
{
    const TranslationBlock* tb   = p;
    const struct tb_desc*   desc = d;

    if ((tb_cflags(tb) & CF_PCREL || tb->pc == desc->s.pc) && tb_page_addr0(tb) == desc->page_addr0
        && tb->cs_base == desc->s.cs_base && tb->flags == desc->s.flags && tb_cflags(tb) == desc->s.cflags)
    {
        /* check next page if needed */
        tb_page_addr_t tb_phys_page1 = tb_page_addr1(tb);
        if (tb_phys_page1 == -1) { return true; }
        else {
            tb_page_addr_t phys_page1;
            vaddr          virt_page1;

            /*
             * We know that the first page matched, and an otherwise valid TB
             * encountered an incomplete instruction at the end of that page,
             * therefore we know that generating a new TB from the current PC
             * must also require reading from the next page -- even if the
             * second pages do not match, and therefore the resulting insn
             * is different for the new TB.  Therefore any exception raised
             * here by the faulting lookup is not premature.
             */
            virt_page1 = TARGET_PAGE_ALIGN(desc->s.pc);
            phys_page1 = get_page_addr_code(desc->env, virt_page1);
            if (tb_phys_page1 == phys_page1) { return true; }
        }
    }
    return false;
}

static TranslationBlock* tb_htable_lookup(CPUState* cpu, TCGTBCPUState s)
{
    tb_page_addr_t phys_pc;
    struct tb_desc desc;
    uint32_t       h;

    desc.s   = s;
    desc.env = cpu_env(cpu);
    phys_pc  = get_page_addr_code(desc.env, s.pc);
    if (phys_pc == -1) { return NULL; }
    desc.page_addr0 = phys_pc;
    h               = tb_hash_func(phys_pc, (s.cflags & CF_PCREL ? 0 : s.pc), s.flags, s.cs_base, s.cflags);
    return qht_lookup_custom(&tb_ctx.htable, &desc, h, tb_lookup_cmp);
}

/**
 * tb_lookup:
 * @cpu: CPU that will execute the returned translation block
 * @pc: guest PC
 * @cs_base: arch-specific value associated with translation block
 * @flags: arch-specific translation block flags
 * @cflags: CF_* flags
 *
 * Look up a translation block inside the QHT using @pc, @cs_base, @flags and
 * @cflags. Uses @cpu's tb_jmp_cache. Might cause an exception, so have a
 * longjmp destination ready.
 *
 * Returns: an existing translation block or NULL.
 */
static inline TranslationBlock* tb_lookup(CPUState* cpu, TCGTBCPUState s)
{
    TranslationBlock* tb;
    CPUJumpCache*     jc;
    uint32_t          hash;

    /* we should never be trying to look up an INVALID tb */
    tcg_debug_assert(!(s.cflags & CF_INVALID));

    hash = tb_jmp_cache_hash_func(s.pc);
    jc   = cpu->tb_jmp_cache;

    tb = qatomic_read(&jc->array[hash].tb);
    if (likely(tb && jc->array[hash].pc == s.pc && tb->cs_base == s.cs_base && tb->flags == s.flags
               && tb_cflags(tb) == s.cflags))
    {
        goto hit;
    }

    tb = tb_htable_lookup(cpu, s);
    if (tb == NULL) { return NULL; }

    jc->array[hash].pc = s.pc;
    qatomic_set(&jc->array[hash].tb, tb);

hit:
    /*
     * As long as tb is not NULL, the contents are consistent.  Therefore,
     * the virtual PC has to match for non-CF_PCREL translations.
     */
    assert((tb_cflags(tb) & CF_PCREL) || tb->pc == s.pc);
    return tb;
}

static void log_cpu_exec(vaddr pc, CPUState* cpu, const TranslationBlock* tb)
{
    if (qemu_log_in_addr_range(pc)) {
        qemu_log_mask(CPU_LOG_EXEC, "Trace %d: %p [%08" PRIx64 "/%016" VADDR_PRIx "/%08x/%08x]\n", cpu->cpu_index,
                      tb->tc.ptr, tb->cs_base, pc, tb->flags, tb->cflags);

        if (qemu_loglevel_mask(CPU_LOG_TB_CPU)) {
            FILE* logfile = qemu_log_trylock();
            if (logfile) {
                int flags = CPU_DUMP_CCOP;

                if (qemu_loglevel_mask(CPU_LOG_TB_FPU)) { flags |= CPU_DUMP_FPU; }
                if (qemu_loglevel_mask(CPU_LOG_TB_VPU)) { flags |= CPU_DUMP_VPU; }
                cpu_dump_state(cpu, logfile, flags);
                qemu_log_unlock(logfile);
            }
        }
    }
}

static bool check_for_breakpoints_slow(CPUState* cpu, vaddr pc, uint32_t* cflags)
{
    CPUBreakpoint* bp;
    bool           match_page = false;

    /*
     * Singlestep overrides breakpoints.
     * This requirement is visible in the record-replay tests, where
     * we would fail to make forward progress in reverse-continue.
     *
     * TODO: gdb singlestep should only override gdb breakpoints,
     * so that one could (gdb) singlestep into the guest kernel's
     * architectural breakpoint handler.
     */
    if (cpu->singlestep_enabled) { return false; }

    QTAILQ_FOREACH (bp, &cpu->breakpoints, entry) {
        /*
         * If we have an exact pc match, trigger the breakpoint.
         * Otherwise, note matches within the page.
         */
        if (pc == bp->pc) {
            bool match_bp = false;

            if (bp->flags & BP_GDB) { match_bp = true; }
            else if (bp->flags & BP_CPU) {
                const TCGCPUOps* tcg_ops = cpu->cc->tcg_ops;
                assert(tcg_ops->debug_check_breakpoint);
                match_bp = tcg_ops->debug_check_breakpoint(cpu);
            }

            if (match_bp) {
                cpu->exception_index = EXCP_DEBUG;
                return true;
            }
        }
        else if (((pc ^ bp->pc) & TARGET_PAGE_MASK) == 0) {
            match_page = true;
        }
    }

    /*
     * Within the same page as a breakpoint, single-step,
     * returning to helper_lookup_tb_ptr after each insn looking
     * for the actual breakpoint.
     *
     * TODO: Perhaps better to record all of the TBs associated
     * with a given virtual page that contains a breakpoint, and
     * then invalidate them when a new overlapping breakpoint is
     * set on the page.  Non-overlapping TBs would not be
     * invalidated, nor would any TB need to be invalidated as
     * breakpoints are removed.
     */
    if (match_page) { *cflags = (*cflags & ~CF_COUNT_MASK) | CF_NO_GOTO_TB | CF_BP_PAGE | 1; }
    return false;
}

static inline bool check_for_breakpoints(CPUState* cpu, vaddr pc, uint32_t* cflags)
{ return unlikely(!QTAILQ_EMPTY(&cpu->breakpoints)) && check_for_breakpoints_slow(cpu, pc, cflags); }

/**
 * helper_lookup_tb_ptr: quick check for next tb
 * @env: current cpu state
 *
 * Look for an existing TB matching the current cpu state.
 * If found, return the code pointer.  If not found, return
 * the tcg epilogue so that we return into cpu_tb_exec.
 */
const void* HELPER(lookup_tb_ptr)(CPUArchState* env)
{
    CPUState*         cpu = env_cpu(env);
    TranslationBlock* tb;

    TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
    s.cflags        = curr_cflags(cpu);

    if (check_for_breakpoints(cpu, s.pc, &s.cflags)) { cpu_loop_exit(cpu); }

    tb = tb_lookup(cpu, s);
    if (tb == NULL) { return tcg_code_gen_epilogue; }

    if (qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC)) { log_cpu_exec(s.pc, cpu, tb); }

    return tb->tc.ptr;
}

/* Return the current PC from CPU, which may be cached in TB. */
static vaddr log_pc(CPUState* cpu, const TranslationBlock* tb)
{
    if (tb_cflags(tb) & CF_PCREL) { return cpu->cc->get_pc(cpu); }
    else {
        return tb->pc;
    }
}

/* Execute a TB, and fix up the CPU state afterwards if necessary */
/*
 * Disable CFI checks.
 * TCG creates binary blobs at runtime, with the transformed code.
 * A TB is a blob of binary code, created at runtime and called with an
 * indirect function call. Since such function did not exist at compile time,
 * the CFI runtime has no way to verify its signature and would fail.
 * TCG is not considered a security-sensitive part of QEMU so this does not
 * affect the impact of CFI in environment with high security requirements
 */
static inline TranslationBlock* QEMU_DISABLE_CFI cpu_tb_exec(CPUState* cpu, TranslationBlock* itb, int* tb_exit)
{
    uintptr_t         ret;
    TranslationBlock* last_tb;
    const void*       tb_ptr = itb->tc.ptr;

    if (qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC)) { log_cpu_exec(log_pc(cpu, itb), cpu, itb); }

    qemu_thread_jit_execute();
    ret = tcg_qemu_tb_exec(cpu_env(cpu), tb_ptr);
    /*
     * TODO: Delay swapping back to the read-write region of the TB
     * until we actually need to modify the TB.  The read-only copy,
     * coming from the rx region, shares the same host TLB entry as
     * the code that executed the exit_tb opcode that arrived here.
     * If we insist on touching both the RX and the RW pages, we
     * double the host TLB pressure.
     */
    last_tb  = tcg_splitwx_to_rw((void*)(ret & ~TB_EXIT_MASK));
    *tb_exit = ret & TB_EXIT_MASK;

#ifdef CONFIG_DEBUG_TCG
    trace_exec_tb_exit(last_tb, *tb_exit);
#endif

    if (*tb_exit > TB_EXIT_IDX1) {
        /* We didn't start executing this TB (eg because the instruction
         * counter hit zero); we must restore the guest PC to the address
         * of the start of the TB.
         */
        CPUClass*        cc      = cpu->cc;
        const TCGCPUOps* tcg_ops = cc->tcg_ops;

        if (tcg_ops->synchronize_from_tb) { tcg_ops->synchronize_from_tb(cpu, last_tb); }
        else {
            tcg_debug_assert(!(tb_cflags(last_tb) & CF_PCREL));
            assert(cc->set_pc);
            cc->set_pc(cpu, last_tb->pc);
        }
        if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
            vaddr pc = log_pc(cpu, last_tb);
            if (qemu_log_in_addr_range(pc)) {
                qemu_log("Stopped execution of TB chain before %p [%016" VADDR_PRIx "]\n", last_tb->tc.ptr, pc);
            }
        }
    }

    /*
     * If gdb single-step, and we haven't raised another exception,
     * raise a debug exception.  Single-step with another exception
     * is handled in cpu_handle_exception.
     */
    if (unlikely(cpu->singlestep_enabled) && cpu->exception_index == -1) {
        cpu->exception_index = EXCP_DEBUG;
        cpu_loop_exit(cpu);
    }

    return last_tb;
}

static void cpu_exec_enter(CPUState* cpu)
{
    const TCGCPUOps* tcg_ops = cpu->cc->tcg_ops;

    if (tcg_ops->cpu_exec_enter) { tcg_ops->cpu_exec_enter(cpu); }
}

static void cpu_exec_exit(CPUState* cpu)
{
    const TCGCPUOps* tcg_ops = cpu->cc->tcg_ops;

    if (tcg_ops->cpu_exec_exit) { tcg_ops->cpu_exec_exit(cpu); }
}

static void cpu_exec_longjmp_cleanup(CPUState* cpu)
{
    /* Non-buggy compilers preserve this; assert the correct value. */
    assert(cpu == current_cpu);

    /*
     * For softmmu, a tlb_fill fault during translation will land here,
     * and we need to release any page locks held.  In system mode we
     * have one tcg_ctx per thread, so we know it was this cpu doing
     * the translation.
     *
     * Alternative 1: Install a cleanup to be called via an exception
     * handling safe longjmp.  It seems plausible that all our hosts
     * support such a thing.  We'd have to properly register unwind info
     * for the JIT for EH, rather that just for GDB.
     *
     * Alternative 2: Set and restore cpu->jmp_env in tb_gen_code to
     * capture the cpu_loop_exit longjmp, perform the cleanup, and
     * jump again to arrive here.
     */
    if (tcg_ctx->gen_tb) {
        tb_unlock_pages(tcg_ctx->gen_tb);
        tcg_ctx->gen_tb = NULL;
    }

    if (bql_locked()) { bql_unlock(); }
    assert_no_pages_locked();
}

void cpu_exec_step_atomic(CPUState* cpu)
{
    TranslationBlock* tb;
    int               tb_exit;

    if (sigsetjmp(cpu->jmp_env, 0) == 0) {
        start_exclusive();
        assert(cpu == current_cpu);
        assert(!cpu->running);
        cpu->running = true;

        TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
        s.cflags        = curr_cflags(cpu);

        /* Execute in a serial context. */
        s.cflags &= ~CF_PARALLEL;
        /* After 1 insn, return and release the exclusive lock. */
        s.cflags |= CF_NO_GOTO_TB | CF_NO_GOTO_PTR | 1;
        /*
         * No need to check_for_breakpoints here.
         * We only arrive in cpu_exec_step_atomic after beginning execution
         * of an insn that includes an atomic operation we can't handle.
         * Any breakpoint for this insn will have been recognized earlier.
         */

        tb = tb_lookup(cpu, s);
        if (tb == NULL) { tb = tb_gen_code(cpu, s); }

        cpu_exec_enter(cpu);
        /* execute the generated code */
#ifdef CONFIG_DEBUG_TCG
        trace_exec_tb(tb, s.pc);
#endif
        cpu_tb_exec(cpu, tb, &tb_exit);
        cpu_exec_exit(cpu);
    }
    else {
        cpu_exec_longjmp_cleanup(cpu);
    }

    /*
     * As we start the exclusive region before codegen we must still
     * be in the region if we longjump out of either the codegen or
     * the execution.
     */
    assert(cpu_in_exclusive_context(cpu));
    cpu->running = false;
    end_exclusive();
}

void tb_set_jmp_target(TranslationBlock* tb, int n, uintptr_t addr)
{
    /*
     * Get the rx view of the structure, from which we find the
     * executable code address, and tb_target_set_jmp_target can
     * produce a pc-relative displacement to jmp_target_addr[n].
     */
    const TranslationBlock* c_tb   = tcg_splitwx_to_rx(tb);
    uintptr_t               offset = tb->jmp_insn_offset[n];
    uintptr_t               jmp_rx = (uintptr_t)tb->tc.ptr + offset;
    uintptr_t               jmp_rw = jmp_rx - tcg_splitwx_diff;

    tb->jmp_target_addr[n] = addr;
    tb_target_set_jmp_target(c_tb, n, jmp_rx, jmp_rw);
}

static inline void tb_add_jump(TranslationBlock* tb, int n, TranslationBlock* tb_next)
{
    uintptr_t old;

    qemu_thread_jit_write();
    assert(n < ARRAY_SIZE(tb->jmp_list_next));
    qemu_spin_lock(&tb_next->jmp_lock);

    /* make sure the destination TB is valid */
    if (tb_next->cflags & CF_INVALID) { goto out_unlock_next; }
    /* Atomically claim the jump destination slot only if it was NULL */
    old = qatomic_cmpxchg(&tb->jmp_dest[n], (uintptr_t)NULL, (uintptr_t)tb_next);
    if (old) { goto out_unlock_next; }

    /* patch the native jump address */
    tb_set_jmp_target(tb, n, (uintptr_t)tb_next->tc.ptr);

    /* add in TB jmp list */
    tb->jmp_list_next[n]   = tb_next->jmp_list_head;
    tb_next->jmp_list_head = (uintptr_t)tb | n;

    qemu_spin_unlock(&tb_next->jmp_lock);

    qemu_log_mask(CPU_LOG_EXEC, "Linking TBs %p index %d -> %p\n", tb->tc.ptr, n, tb_next->tc.ptr);
    return;

out_unlock_next:
    qemu_spin_unlock(&tb_next->jmp_lock);
}

static inline bool cpu_handle_halt(CPUState* cpu)
{
    if (cpu->halted) {
        const TCGCPUOps* tcg_ops    = cpu->cc->tcg_ops;
        bool             leave_halt = tcg_ops->cpu_exec_halt(cpu);

        if (!leave_halt) { return true; }

        cpu->halted = 0;
    }

    return false;
}

static inline void cpu_handle_debug_exception(CPUState* cpu)
{
    const TCGCPUOps* tcg_ops = cpu->cc->tcg_ops;
    CPUWatchpoint*   wp;

    if (!cpu->watchpoint_hit) {
        QTAILQ_FOREACH (wp, &cpu->watchpoints, entry) { wp->flags &= ~BP_WATCHPOINT_HIT; }
    }

    if (tcg_ops->debug_excp_handler) { tcg_ops->debug_excp_handler(cpu); }
}

static inline bool cpu_handle_exception(CPUState* cpu, int* ret)
{
    if (cpu->exception_index < 0) { return false; }

    if (cpu->exception_index >= EXCP_INTERRUPT) {
        /* exit request from the cpu execution loop */
        *ret = cpu->exception_index;
        if (*ret == EXCP_DEBUG) { cpu_handle_debug_exception(cpu); }
        cpu->exception_index = -1;
        return true;
    }

    const TCGCPUOps* tcg_ops = cpu->cc->tcg_ops;

    tcg_ops->do_interrupt(cpu);
    cpu->exception_index = -1;

    if (unlikely(cpu->singlestep_enabled)) {
        /*
         * After processing the exception, ensure an EXCP_DEBUG is
         * raised when single-stepping so that GDB doesn't miss the
         * next instruction.
         */
        *ret = EXCP_DEBUG;
        cpu_handle_debug_exception(cpu);
        return true;
    }

    return false;
}

void tcg_kick_vcpu_thread(CPUState* cpu)
{
    /*
     * Ensure cpu_exec will see the reason why the exit request was set.
     * FIXME: this is not always needed.  Other accelerators instead
     * read interrupt_request and set exit_request on demand from the
     * CPU thread; see kvm_arch_pre_run() for example.
     */
    qatomic_store_release(&cpu->exit_request, true);

    /* Ensure cpu_exec will see the exit request after TCG has exited.  */
    qatomic_store_release(&cpu->neg.tb_exit_request, true);
}

static inline bool cpu_handle_interrupt(CPUState* cpu, TranslationBlock** last_tb)
{
    /*
     * If we have requested custom cflags with CF_NOIRQ we should
     * skip checking here. Any pending interrupts will get picked up
     * by the next TB we execute under normal cflags.
     */
    if (cpu->cflags_next_tb != -1 && cpu->cflags_next_tb & CF_NOIRQ) { return false; }

    /* Clear the interrupt flag now since we're processing
     * cpu->interrupt_request and cpu->exit_request.
     * Ensure zeroing happens before reading cpu->exit_request or
     * cpu->interrupt_request (see also store-release in
     * tcg_kick_vcpu_thread())
     */
    qatomic_set_mb(&cpu->neg.tb_exit_request, false);

    if (unlikely(cpu_test_interrupt(cpu, ~0))) {
        /* EXITTB alone is an atomic clear and a local store. */
        if (!cpu_test_interrupt(cpu, ~CPU_INTERRUPT_EXITTB)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_EXITTB);
            *last_tb = NULL;
            goto check_exit_request;
        }

        bql_lock();
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_DEBUG)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_DEBUG);
            cpu->exception_index = EXCP_DEBUG;
            bql_unlock();
            return true;
        }
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_HALT)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_HALT);
            cpu->halted          = 1;
            cpu->exception_index = EXCP_HLT;
            bql_unlock();
            return true;
        }
        else {
            const TCGCPUOps* tcg_ops           = cpu->cc->tcg_ops;
            int              interrupt_request = cpu->interrupt_request;

            if (cpu_test_interrupt(cpu, CPU_INTERRUPT_RESET)) {
                tcg_ops->cpu_exec_reset(cpu);
                bql_unlock();
                return true;
            }

            if (unlikely(cpu->singlestep_enabled & SSTEP_NOIRQ)) {
                /* Mask out external interrupts for this step. */
                interrupt_request &= ~CPU_INTERRUPT_SSTEP_MASK;
            }

            /*
             * The target hook has 3 exit conditions:
             * False when the interrupt isn't processed,
             * True when it is, and we should restart on a new TB,
             * and via longjmp via cpu_loop_exit.
             */
            if (tcg_ops->cpu_exec_interrupt(cpu, interrupt_request)) {
                /*
                 * After processing the interrupt, ensure an EXCP_DEBUG is
                 * raised when single-stepping so that GDB doesn't miss the
                 * next instruction.
                 */
                if (unlikely(cpu->singlestep_enabled)) {
                    cpu->exception_index = EXCP_DEBUG;
                    bql_unlock();
                    return true;
                }
                cpu->exception_index = -1;
                *last_tb             = NULL;
            }
        }
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_EXITTB)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_EXITTB);
            /* ensure that no TB jump will be modified as
               the program flow was changed */
            *last_tb = NULL;
        }

        /* If we exit via cpu_loop_exit/longjmp it is reset in cpu_exec */
        bql_unlock();
    }

check_exit_request:
    /*
     * Finally, check if we need to exit to the main loop.
     * The corresponding store-release is in cpu_exit.
     */
    if (unlikely(qatomic_load_acquire(&cpu->exit_request))) {
        if (cpu->exception_index == -1) { cpu->exception_index = EXCP_INTERRUPT; }
        return true;
    }

    return false;
}

static inline void cpu_loop_exec_tb(CPUState* cpu, TranslationBlock* tb, vaddr pc, TranslationBlock** last_tb,
                                    int* tb_exit)
{
#ifdef CONFIG_DEBUG_TCG
    trace_exec_tb(tb, pc);
#endif
    tb = cpu_tb_exec(cpu, tb, tb_exit);
    if (*tb_exit != TB_EXIT_REQUESTED) {
        *last_tb = tb;
        return;
    }

    *last_tb = NULL;
    if (cpu_loop_exit_requested(cpu)) {
        /* Something asked us to stop executing chained TBs; just
         * continue round the main loop. Whatever requested the exit
         * will also have set something else (eg exit_request or
         * interrupt_request) which will be handled by
         * cpu_handle_interrupt.  cpu_handle_interrupt will also
         * clear cpu->neg.tb_exit_request.
         */
        return;
    }

    assert_not_reached();
}

/* main execution loop */

static int __attribute__((noinline)) cpu_exec_loop(CPUState* cpu)
{
    int ret;

    /* if an exception is pending, we execute it here */
    while (!cpu_handle_exception(cpu, &ret)) {
        TranslationBlock* last_tb = NULL;
        int               tb_exit = 0;

        while (!cpu_handle_interrupt(cpu, &last_tb)) {
            TranslationBlock* tb;
            TCGTBCPUState     s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
            s.cflags            = cpu->cflags_next_tb;

            /*
             * When requested, use an exact setting for cflags for the next
             * execution.  This is used for precise smc and stop-after-access
             * watchpoints.  Since this request should never
             * have CF_INVALID set, -1 is a convenient invalid value that
             * does not require tcg headers for cpu_common_reset.
             */
            if (s.cflags == -1) { s.cflags = curr_cflags(cpu); }
            else {
                cpu->cflags_next_tb = -1;
            }

            if (check_for_breakpoints(cpu, s.pc, &s.cflags)) { break; }

            tb = tb_lookup(cpu, s);
            if (tb == NULL) {
                CPUJumpCache* jc;
                uint32_t      h;

                tb = tb_gen_code(cpu, s);

                /*
                 * We add the TB in the virtual pc hash table
                 * for the fast lookup
                 */
                h               = tb_jmp_cache_hash_func(s.pc);
                jc              = cpu->tb_jmp_cache;
                jc->array[h].pc = s.pc;
                qatomic_set(&jc->array[h].tb, tb);
            }

            /*
             * We don't take care of direct jumps when address mapping
             * changes in system emulation.  So it's not safe to make a
             * direct jump to a TB spanning two pages because the mapping
             * for the second page can change.
             */
            if (tb_page_addr1(tb) != -1) { last_tb = NULL; }

            /* See if we can patch the calling TB. */
            if (last_tb) { tb_add_jump(last_tb, tb_exit, tb); }

            cpu_loop_exec_tb(cpu, tb, s.pc, &last_tb, &tb_exit);
        }
    }
    return ret;
}

static int cpu_exec_setjmp(CPUState* cpu)
{
    /* Prepare setjmp context for exception handling. */
    if (unlikely(sigsetjmp(cpu->jmp_env, 0) != 0)) { cpu_exec_longjmp_cleanup(cpu); }

    return cpu_exec_loop(cpu);
}

int cpu_exec(CPUState* cpu)
{
    int ret;

    current_cpu = cpu;

    if (cpu_handle_halt(cpu)) { return EXCP_HALTED; }

    RCU_READ_LOCK_GUARD();
    cpu_exec_enter(cpu);

    ret = cpu_exec_setjmp(cpu);

    cpu_exec_exit(cpu);
    return ret;
}

bool tcg_exec_realizefn(CPUState* cpu, Error** errp)
{
    static bool tcg_target_initialized;

    if (!tcg_target_initialized) {
        /* Check mandatory TCGCPUOps handlers */
        const TCGCPUOps* tcg_ops = cpu->cc->tcg_ops;
        assert(tcg_ops->cpu_exec_halt);
        assert(tcg_ops->cpu_exec_interrupt);
        assert(tcg_ops->cpu_exec_reset);
        assert(tcg_ops->pointer_wrap);
        assert(tcg_ops->translate_code);
        assert(tcg_ops->get_tb_cpu_state);
        assert(tcg_ops->mmu_index);
        tcg_ops->initialize();
        tcg_target_initialized = true;
    }

    cpu->tb_jmp_cache = g_new0(CPUJumpCache, 1);
    tlb_init(cpu);
    tcg_iommu_init_notifier_list(cpu);

    return true;
}

/* undo the initializations in reverse order */
void tcg_exec_unrealizefn(CPUState* cpu)
{
    tcg_iommu_free_notifier_list(cpu);

    tlb_destroy(cpu);
    g_free_rcu(cpu->tb_jmp_cache, rcu);
}
