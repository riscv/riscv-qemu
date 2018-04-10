/*
 * RISC-V Control and Status Registers.
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2017-2018 SiFive, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "qemu/main-loop.h"
#include "exec/exec-all.h"


/* Control and Status Register function table forward declaration */

typedef int (*riscv_csr_read_fn)(CPURISCVState *env, int csrno,
    target_ulong *ret_value);
typedef int (*riscv_csr_write_fn)(CPURISCVState *env, int csrno,
    target_ulong new_value);
typedef int (*riscv_csr_op_fn)(CPURISCVState *env, int csrno,
    target_ulong *ret_value, target_ulong new_value, target_ulong write_mask);

typedef struct {
    riscv_csr_read_fn read;
    riscv_csr_write_fn write;
    riscv_csr_op_fn op;
} riscv_csr_operations;

static const riscv_csr_operations csr_ops[];


/* User Floating-Point CSRs */

static int read_fflags(CPURISCVState *env, int csrno, target_ulong *val)
{
#if !defined(CONFIG_USER_ONLY)
    if (!(env->mstatus & MSTATUS_FS)) {
        return -1;
    }
#endif
    *val = cpu_riscv_get_fflags(env);
    return 0;
}

static int write_fflags(CPURISCVState *env, int csrno, target_ulong val)
{
#if !defined(CONFIG_USER_ONLY)
    if (!(env->mstatus & MSTATUS_FS)) {
        return -1;
    }
    env->mstatus |= MSTATUS_FS;
#endif
    cpu_riscv_set_fflags(env, val & (FSR_AEXC >> FSR_AEXC_SHIFT));
    return 0;
}

static int read_frm(CPURISCVState *env, int csrno, target_ulong *val)
{
#if !defined(CONFIG_USER_ONLY)
    if (!(env->mstatus & MSTATUS_FS)) {
        return -1;
    }
#endif
    *val = env->frm;
    return 0;
}

static int write_frm(CPURISCVState *env, int csrno, target_ulong val)
{
#if !defined(CONFIG_USER_ONLY)
    if (!(env->mstatus & MSTATUS_FS)) {
        return -1;
    }
    env->mstatus |= MSTATUS_FS;
#endif
    env->frm = val & (FSR_RD >> FSR_RD_SHIFT);
    return 0;
}

static int read_fcsr(CPURISCVState *env, int csrno, target_ulong *val)
{
#if !defined(CONFIG_USER_ONLY)
    if (!(env->mstatus & MSTATUS_FS)) {
        return -1;
    }
#endif
    *val = (cpu_riscv_get_fflags(env) << FSR_AEXC_SHIFT)
        | (env->frm << FSR_RD_SHIFT);
    return 0;
}

static int write_fcsr(CPURISCVState *env, int csrno, target_ulong val)
{
#if !defined(CONFIG_USER_ONLY)
    if (!(env->mstatus & MSTATUS_FS)) {
        return -1;
    }
    env->mstatus |= MSTATUS_FS;
#endif
    env->frm = (val & FSR_RD) >> FSR_RD_SHIFT;
    cpu_riscv_set_fflags(env, (val & FSR_AEXC) >> FSR_AEXC_SHIFT);
    return 0;
}

/* User Timers and Counters */

static int counter_enabled(CPURISCVState *env, int csrno)
{
#ifndef CONFIG_USER_ONLY
    target_ulong ctr_en = env->priv == PRV_U ? env->mucounteren :
                          env->priv == PRV_S ? env->mscounteren : -1U;
#else
    target_ulong ctr_en = -1;
#endif
    return (ctr_en >> (csrno & 31)) & 1;
}

#if !defined(CONFIG_USER_ONLY)
static int read_zero_counter(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (!counter_enabled(env, csrno)) {
        return -1;
    }
    *val = 0;
    return 0;
}
#endif

static int read_instret(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (!counter_enabled(env, csrno)) {
        return -1;
    }
#if !defined(CONFIG_USER_ONLY)
    if (use_icount) {
        *val = cpu_get_icount();
    } else {
        *val = cpu_get_host_ticks();
    }
#else
    *val = cpu_get_host_ticks();
#endif
    return 0;
}

#if defined(TARGET_RISCV32)
static int read_instreth(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (!counter_enabled(env, csrno)) {
        return -1;
    }
#if !defined(CONFIG_USER_ONLY)
    if (use_icount) {
        *val = cpu_get_icount() >> 32;
    } else {
        *val = cpu_get_host_ticks() >> 32;
    }
#else
    *val = cpu_get_host_ticks() >> 32;
#endif
    return 0;
}
#endif

#if defined(CONFIG_USER_ONLY)

static int read_time(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = cpu_get_host_ticks();
    return 0;
}

#if defined(TARGET_RISCV32)
static int read_timeh(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = cpu_get_host_ticks() >> 32;
    return 0;
}
#endif

#else

/* Machine constants */

static const target_ulong delegable_ints = MIP_SSIP | MIP_STIP | MIP_SEIP;
static const target_ulong all_ints = MIP_SSIP | MIP_STIP | MIP_SEIP |
    MIP_MSIP | MIP_MTIP;
static const target_ulong delegable_excps =
    (1ULL << (RISCV_EXCP_INST_ADDR_MIS)) |
    (1ULL << (RISCV_EXCP_INST_ACCESS_FAULT)) |
    (1ULL << (RISCV_EXCP_ILLEGAL_INST)) |
    (1ULL << (RISCV_EXCP_BREAKPOINT)) |
    (1ULL << (RISCV_EXCP_LOAD_ADDR_MIS)) |
    (1ULL << (RISCV_EXCP_LOAD_ACCESS_FAULT)) |
    (1ULL << (RISCV_EXCP_STORE_AMO_ADDR_MIS)) |
    (1ULL << (RISCV_EXCP_STORE_AMO_ACCESS_FAULT)) |
    (1ULL << (RISCV_EXCP_U_ECALL)) |
    (1ULL << (RISCV_EXCP_S_ECALL)) |
    (1ULL << (RISCV_EXCP_H_ECALL)) |
    (1ULL << (RISCV_EXCP_M_ECALL)) |
    (1ULL << (RISCV_EXCP_INST_PAGE_FAULT)) |
    (1ULL << (RISCV_EXCP_LOAD_PAGE_FAULT)) |
    (1ULL << (RISCV_EXCP_STORE_PAGE_FAULT));
static const target_ulong sstatus_v1_9_mask = SSTATUS_SIE | SSTATUS_SPIE |
    SSTATUS_UIE | SSTATUS_UPIE | SSTATUS_SPP | SSTATUS_FS | SSTATUS_XS |
    SSTATUS_SUM | SSTATUS_SD;
static const target_ulong sstatus_v1_10_mask = SSTATUS_SIE | SSTATUS_SPIE |
    SSTATUS_UIE | SSTATUS_UPIE | SSTATUS_SPP | SSTATUS_FS | SSTATUS_XS |
    SSTATUS_SUM | SSTATUS_MXR | SSTATUS_SD;

#if defined(TARGET_RISCV32)
static const char valid_vm_1_09[16] = {
    [VM_1_09_MBARE] = 1,
    [VM_1_09_SV32] = 1,
};
static const char valid_vm_1_10[16] = {
    [VM_1_10_MBARE] = 1,
    [VM_1_10_SV32] = 1
};
#elif defined(TARGET_RISCV64)
static const char valid_vm_1_09[16] = {
    [VM_1_09_MBARE] = 1,
    [VM_1_09_SV39] = 1,
    [VM_1_09_SV48] = 1,
};
static const char valid_vm_1_10[16] = {
    [VM_1_10_MBARE] = 1,
    [VM_1_10_SV39] = 1,
    [VM_1_10_SV48] = 1,
    [VM_1_10_SV57] = 1
};
#endif

/* Machine Information Registers */

static int read_zero(CPURISCVState *env, int csrno, target_ulong *val)
{
    return *val = 0;
}

static int read_mhartid(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mhartid;
    return 0;
}

/* Machine Trap Setup */

static int read_mstatus(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mstatus;
    return 0;
}

static int validate_vm(CPURISCVState *env, target_ulong vm)
{
    return (env->priv_ver >= PRIV_VERSION_1_10_0) ?
        valid_vm_1_10[vm & 0xf] : valid_vm_1_09[vm & 0xf];
}

static int write_mstatus(CPURISCVState *env, int csrno, target_ulong val)
{
    target_ulong mstatus = env->mstatus;
    target_ulong mask = 0;
    target_ulong mpp = get_field(val, MSTATUS_MPP);

    /* flush tlb on mstatus fields that affect VM */
    if (env->priv_ver <= PRIV_VERSION_1_09_1) {
        if ((val ^ mstatus) & (MSTATUS_MXR | MSTATUS_MPP |
                MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_VM)) {
            tlb_flush(CPU(riscv_env_get_cpu(env)));
        }
        mask = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_MIE | MSTATUS_MPIE |
            MSTATUS_SPP | MSTATUS_FS | MSTATUS_MPRV | MSTATUS_SUM |
            MSTATUS_MPP | MSTATUS_MXR |
            (validate_vm(env, get_field(val, MSTATUS_VM)) ?
                MSTATUS_VM : 0);
    }
    if (env->priv_ver >= PRIV_VERSION_1_10_0) {
        if ((val ^ mstatus) & (MSTATUS_MXR | MSTATUS_MPP |
                MSTATUS_MPRV | MSTATUS_SUM)) {
            tlb_flush(CPU(riscv_env_get_cpu(env)));
        }
        mask = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_MIE | MSTATUS_MPIE |
            MSTATUS_SPP | MSTATUS_FS | MSTATUS_MPRV | MSTATUS_SUM |
            MSTATUS_MPP | MSTATUS_MXR;
    }

    /* silenty discard mstatus.mpp writes for unsupported modes */
    if (mpp == PRV_H ||
        (!riscv_has_ext(env, RVS) && mpp == PRV_S) ||
        (!riscv_has_ext(env, RVU) && mpp == PRV_U)) {
        mask &= ~MSTATUS_MPP;
    }

    mstatus = (mstatus & ~mask) | (val & mask);

    /* Note: this is a workaround for an issue where mstatus.FS
       does not report dirty after floating point operations
       that modify floating point state. This workaround is
       technically compliant with the RISC-V Privileged
       specification as it is legal to return only off, or dirty.
       at the expense of extra floating point save/restore. */

    /* FP is always dirty or off */
    if (mstatus & MSTATUS_FS) {
        mstatus |= MSTATUS_FS;
    }

    int dirty = ((mstatus & MSTATUS_FS) == MSTATUS_FS) |
                ((mstatus & MSTATUS_XS) == MSTATUS_XS);
    mstatus = set_field(mstatus, MSTATUS_SD, dirty);
    env->mstatus = mstatus;

    return 0;
}

static int read_misa(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->misa;
    return 0;
}

static int read_medeleg(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->medeleg;
    return 0;
}

static int write_medeleg(CPURISCVState *env, int csrno, target_ulong val)
{
    env->medeleg = (env->medeleg & ~delegable_excps) | (val & delegable_excps);
    return 0;
}

static int read_mideleg(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mideleg;
    return 0;
}

static int write_mideleg(CPURISCVState *env, int csrno, target_ulong val)
{
    env->mideleg = (env->mideleg & ~delegable_ints) | (val & delegable_ints);
    return 0;
}

static int read_mie(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mie;
    return 0;
}

static int write_mie(CPURISCVState *env, int csrno, target_ulong val)
{
    env->mie = (env->mie & ~all_ints) | (val & all_ints);
    return 0;
}

static int read_mtvec(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mtvec;
    return 0;
}

static int write_mtvec(CPURISCVState *env, int csrno, target_ulong val)
{
    /* we do not support vectored traps for asynchrounous interrupts */
    if ((val & 3) == 0) {
        env->mtvec = val >> 2 << 2;
    }
    return 0;
}

static int read_mcounteren(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (env->priv_ver < PRIV_VERSION_1_10_0) {
        return -1;
    }
    *val = env->mcounteren;
    return 0;
}

static int write_mcounteren(CPURISCVState *env, int csrno, target_ulong val)
{
    if (env->priv_ver < PRIV_VERSION_1_10_0) {
        return -1;
    }
    env->mcounteren = val;
    return 0;
}

static int read_mscounteren(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (env->priv_ver > PRIV_VERSION_1_09_1) {
        return -1;
    }
    *val = env->mcounteren;
    return 0;
}

static int write_mscounteren(CPURISCVState *env, int csrno, target_ulong val)
{
    if (env->priv_ver > PRIV_VERSION_1_09_1) {
        return -1;
    }
    env->mcounteren = val;
    return 0;
}

static int read_mucounteren(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (env->priv_ver > PRIV_VERSION_1_09_1) {
        return -1;
    }
    *val = env->scounteren;
    return 0;
}

static int write_mucounteren(CPURISCVState *env, int csrno, target_ulong val)
{
    if (env->priv_ver > PRIV_VERSION_1_09_1) {
        return -1;
    }
    env->scounteren = val;
    return 0;
}

/* Machine Trap Handling */

static int read_mscratch(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mscratch;
    return 0;
}

static int write_mscratch(CPURISCVState *env, int csrno, target_ulong val)
{
    env->mscratch = val;
    return 0;
}

static int read_mepc(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mepc;
    return 0;
}

static int write_mepc(CPURISCVState *env, int csrno, target_ulong val)
{
    env->mepc = val;
    return 0;
}

static int read_mcause(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mcause;
    return 0;
}

static int write_mcause(CPURISCVState *env, int csrno, target_ulong val)
{
    env->mcause = val;
    return 0;
}

static int read_mbadaddr(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mbadaddr;
    return 0;
}

static int write_mbadaddr(CPURISCVState *env, int csrno, target_ulong val)
{
    env->mbadaddr = val;
    return 0;
}

static int rmw_mip(CPURISCVState *env, int csrno, target_ulong *ret_value,
                   target_ulong new_value, target_ulong write_mask)
{
    RISCVCPU *cpu = riscv_env_get_cpu(env);
    target_ulong mask = write_mask & delegable_ints;
    uint32_t old_mip;

    /* We can't allow the supervisor to control SEIP as this would allow the
     * supervisor to clear a pending external interrupt which will result in
     * lost a interrupt in the case a PLIC is attached. The SEIP bit must be
     * hardware controlled when a PLIC is attached. This should be an option
     * for CPUs with software-delegated Supervisor External Interrupts. */
    mask &= ~MIP_SEIP;

    if (mask) {
        qemu_mutex_lock_iothread();
        old_mip = riscv_set_local_interrupt(cpu, mask, (new_value & mask));
        qemu_mutex_unlock_iothread();
    } else {
        old_mip = atomic_read(&env->mip);
    }

    if (ret_value) {
        *ret_value = old_mip;
    }

    return 0;
}

/* Supervisor Trap Setup */

static int read_sstatus(CPURISCVState *env, int csrno, target_ulong *val)
{
    target_ulong mask = ((env->priv_ver >= PRIV_VERSION_1_10_0) ?
                         sstatus_v1_10_mask : sstatus_v1_9_mask);
    *val = env->mstatus & mask;
    return 0;
}

static int write_sstatus(CPURISCVState *env, int csrno, target_ulong val)
{
    target_ulong mask = ((env->priv_ver >= PRIV_VERSION_1_10_0) ?
                         sstatus_v1_10_mask : sstatus_v1_9_mask);
    target_ulong newval = (env->mstatus & ~mask) | (val & mask);
    return write_mstatus(env, CSR_MSTATUS, newval);
}

static int read_sie(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->mie & env->mideleg;
    return 0;
}

static int write_sie(CPURISCVState *env, int csrno, target_ulong val)
{
    target_ulong newval = (env->mie & ~env->mideleg) | (val & env->mideleg);
    return write_mie(env, CSR_MIE, newval);
}

static int read_stvec(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->stvec;
    return 0;
}

static int write_stvec(CPURISCVState *env, int csrno, target_ulong val)
{
    /* we do not support vectored traps for asynchrounous interrupts */
    if ((val & 3) == 0) {
        env->stvec = val >> 2 << 2;
    }
    return 0;
}

static int read_scounteren(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (env->priv_ver < PRIV_VERSION_1_10_0) {
        return -1;
    }
    *val = env->scounteren;
    return 0;
}

static int write_scounteren(CPURISCVState *env, int csrno, target_ulong val)
{
    if (env->priv_ver < PRIV_VERSION_1_10_0) {
        return -1;
    }
    env->scounteren = val;
    return 0;
}

/* Supervisor Trap Handling */

static int read_sscratch(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->sscratch;
    return 0;
}

static int write_sscratch(CPURISCVState *env, int csrno, target_ulong val)
{
    env->sscratch = val;
    return 0;
}

static int read_sepc(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->sepc;
    return 0;
}

static int write_sepc(CPURISCVState *env, int csrno, target_ulong val)
{
    env->sepc = val;
    return 0;
}

static int read_scause(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->scause;
    return 0;
}

static int write_scause(CPURISCVState *env, int csrno, target_ulong val)
{
    env->scause = val;
    return 0;
}

static int read_sbadaddr(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->sbadaddr;
    return 0;
}

static int write_sbadaddr(CPURISCVState *env, int csrno, target_ulong val)
{
    env->sbadaddr = val;
    return 0;
}

static int rmw_sip(CPURISCVState *env, int csrno, target_ulong *ret_value,
                   target_ulong new_value, target_ulong write_mask)
{
    return rmw_mip(env, CSR_MSTATUS, ret_value, new_value,
                   write_mask & env->mideleg);
}

/* Supervisor Protection and Translation */

static int read_satp(CPURISCVState *env, int csrno, target_ulong *val)
{
    if (!riscv_feature(env, RISCV_FEATURE_MMU)) {
        *val = 0;
    } else if (env->priv_ver >= PRIV_VERSION_1_10_0) {
        *val = env->satp;
    } else {
        *val = env->sptbr;
    }
    return 0;
}

static int write_satp(CPURISCVState *env, int csrno, target_ulong val)
{
    if (!riscv_feature(env, RISCV_FEATURE_MMU)) {
        return 0;
    }
    if (env->priv_ver <= PRIV_VERSION_1_09_1 && (val ^ env->sptbr)) {
        tlb_flush(CPU(riscv_env_get_cpu(env)));
        env->sptbr = val & (((target_ulong)
            1 << (TARGET_PHYS_ADDR_SPACE_BITS - PGSHIFT)) - 1);
    }
    if (env->priv_ver >= PRIV_VERSION_1_10_0 &&
        validate_vm(env, get_field(val, SATP_MODE)) &&
        ((val ^ env->satp) & (SATP_MODE | SATP_ASID | SATP_PPN)))
    {
        tlb_flush(CPU(riscv_env_get_cpu(env)));
        env->satp = val;
    }
    return 0;
}

/* Physical Memory Protection */

static int read_pmpcfg(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = pmpcfg_csr_read(env, csrno - CSR_PMPCFG0);
    return 0;
}

static int write_pmpcfg(CPURISCVState *env, int csrno, target_ulong val)
{
    pmpcfg_csr_write(env, csrno - CSR_PMPCFG0, val);
    return 0;
}

static int read_pmpaddr(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = pmpaddr_csr_read(env, csrno - CSR_PMPADDR0);
    return 0;
}

static int write_pmpaddr(CPURISCVState *env, int csrno, target_ulong val)
{
    pmpaddr_csr_write(env, csrno - CSR_PMPADDR0, val);
    return 0;
}

#endif

/*
 * riscv_csrrw - read and/or update control and status register
 *
 * csrr   <->  riscv_csrrw(env, csrno, ret_value, 0, 0);
 * csrrw  <->  riscv_csrrw(env, csrno, ret_value, value, -1);
 * csrrs  <->  riscv_csrrw(env, csrno, ret_value, -1, value);
 * csrrc  <->  riscv_csrrw(env, csrno, ret_value, 0, value);
 */

int riscv_csrrw(CPURISCVState *env, int csrno, target_ulong *ret_value,
                target_ulong new_value, target_ulong write_mask)
{
    int ret;
    target_ulong old_value;

    /* check privileges and return -1 if check fails */
#if !defined(CONFIG_USER_ONLY)
    int csr_priv = get_field(csrno, 0x300);
    int read_only = get_field(csrno, 0xC00) == 3;
    if ((write_mask && read_only) || (env->priv < csr_priv)) {
        return -1;
    }
#endif

    /* execute combined read/write operation if it exists */
    if (csr_ops[csrno].op) {
        return csr_ops[csrno].op(env, csrno, ret_value, new_value, write_mask);
    }

    /* if no accessor exists then return failure */
    if (!csr_ops[csrno].read) {
        return -1;
    }

    /* read old value */
    ret = csr_ops[csrno].read(env, csrno, &old_value);
    if (ret < 0) {
        return ret;
    }

    /* write value if writable and write mask set, otherwise drop writes */
    if (write_mask) {
        new_value = (old_value & ~write_mask) | (new_value & write_mask);
        if (csr_ops[csrno].write) {
            ret = csr_ops[csrno].write(env, csrno, new_value);
            if (ret < 0) {
                return ret;
            }
        }
    }

    /* return old value */
    if (ret_value) {
        *ret_value = old_value;
    }

    return 0;
}

/* Control and Status Register function table */

static const riscv_csr_operations csr_ops[0xfff] = {
    /* User Floating-Point CSRs */
    [CSR_FFLAGS] =              { read_fflags,      write_fflags      },
    [CSR_FRM] =                 { read_frm,         write_frm         },
    [CSR_FCSR] =                { read_fcsr,        write_fcsr        },

    /* User Timers and Counters */
    [CSR_CYCLE] =               { read_instret                        },
    [CSR_INSTRET] =             { read_instret                        },
#if defined(TARGET_RISCV32)
    [CSR_CYCLEH] =              { read_instreth                       },
    [CSR_INSTRETH] =            { read_instreth                       },
#endif

    /* User-level time CSRs are only available in linux-user
     * In privileged mode, the monitor emulates these CSRs */
#if defined(CONFIG_USER_ONLY)
    [CSR_TIME] =                { read_time                           },
#if defined(TARGET_RISCV32)
    [CSR_TIMEH] =               { read_timeh                          },
#endif
#endif

#if !defined(CONFIG_USER_ONLY)
    /* Machine Timers and Counters */
    [CSR_MCYCLE] =              { read_instret                        },
    [CSR_MINSTRET] =            { read_instret                        },
#if defined(TARGET_RISCV32)
    [CSR_MCYCLEH] =             { read_instreth                       },
    [CSR_MINSTRETH] =           { read_instreth                       },
#endif

    /* Machine Information Registers */
    [CSR_MVENDORID] =           { read_zero                           },
    [CSR_MARCHID] =             { read_zero                           },
    [CSR_MIMPID] =              { read_zero                           },
    [CSR_MHARTID] =             { read_mhartid                        },

    /* Machine Trap Setup */
    [CSR_MSTATUS] =             { read_mstatus,     write_mstatus     },
    [CSR_MISA] =                { read_misa                           },
    [CSR_MIDELEG] =             { read_mideleg,     write_mideleg     },
    [CSR_MEDELEG] =             { read_medeleg,     write_medeleg     },
    [CSR_MIE] =                 { read_mie,         write_mie         },
    [CSR_MTVEC] =               { read_mtvec,       write_mtvec       },
    [CSR_MCOUNTEREN] =          { read_mcounteren,  write_mcounteren  },

    /* Legacy Counter Setup (priv v1.9.1) */
    [CSR_MUCOUNTEREN] =         { read_mucounteren, write_mucounteren },
    [CSR_MSCOUNTEREN] =         { read_mscounteren, write_mscounteren },

    /* Machine Trap Handling */
    [CSR_MSCRATCH] =            { read_mscratch,    write_mscratch    },
    [CSR_MEPC] =                { read_mepc,        write_mepc        },
    [CSR_MCAUSE] =              { read_mcause,      write_mcause      },
    [CSR_MBADADDR] =            { read_mbadaddr,    write_mbadaddr    },
    [CSR_MIP] =                 { NULL,     NULL,     rmw_mip         },

    /* Supervisor Trap Setup */
    [CSR_SSTATUS] =             { read_sstatus,     write_sstatus     },
    [CSR_SIE] =                 { read_sie,         write_sie         },
    [CSR_STVEC] =               { read_stvec,       write_stvec       },
    [CSR_SCOUNTEREN] =          { read_scounteren,  write_scounteren  },

    /* Supervisor Trap Handling */
    [CSR_SSCRATCH] =            { read_sscratch,    write_sscratch    },
    [CSR_SEPC] =                { read_sepc,        write_sepc        },
    [CSR_SCAUSE] =              { read_scause,      write_scause      },
    [CSR_SBADADDR] =            { read_sbadaddr,    write_sbadaddr    },
    [CSR_SIP] =                 { NULL,     NULL,     rmw_sip         },

    /* Supervisor Protection and Translation */
    [CSR_SATP] =                { read_satp,        write_satp        },

    /* Physical Memory Protection */
    [CSR_PMPCFG0  ... CSR_PMPADDR9] =  { read_pmpcfg,  write_pmpcfg   },
    [CSR_PMPADDR0 ... CSR_PMPADDR15] = { read_pmpaddr, write_pmpaddr  },

    /* Performance Counters */
    [CSR_HPMCOUNTER3   ... CSR_HPMCOUNTER31] =    { read_zero_counter },
    [CSR_MHPMCOUNTER3  ... CSR_MHPMCOUNTER31] =   { read_zero         },
    [CSR_MHPMEVENT3    ... CSR_MHPMEVENT31] =     { read_zero         },
#if defined(TARGET_RISCV32)
    [CSR_HPMCOUNTER3H  ... CSR_HPMCOUNTER31H] =   { read_zero_counter },
    [CSR_MHPMCOUNTER3H ... CSR_MHPMCOUNTER31H] =  { read_zero         },
#endif
#endif
};
