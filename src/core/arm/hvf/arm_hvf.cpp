// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <utility>
#include <cstring>

#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>

#include "common/assert.h"
#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "common/settings.h"
#include "core/arm/hvf/arm_hvf.h"
#include "core/arm/hvf/hvf_vm.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

namespace Core {

namespace {

constexpr u64 PstateEl1h = 0x3c5;
constexpr u64 PstateModeMask = 0xf;
constexpr u64 PstateFlags = 0xF000'0000;

// Exception classes.
constexpr u64 EcWfx = 0x01;
constexpr u64 EcSvc = 0x15;
constexpr u64 EcHvc = 0x16;
constexpr u64 EcSystemRegister = 0x18;
constexpr u64 EcInstructionAbort = 0x20;
constexpr u64 EcDataAbort = 0x24;
constexpr u64 EcBreakpoint = 0x3c;

// Trapped system registers, as the syndrome encodes them (op0, op2, op1, CRn, CRm).
constexpr u64 SystemRegisterMask = 0x3ffc1e;
constexpr u64 SystemRegister(u64 op0, u64 op1, u64 crn, u64 crm, u64 op2) {
    return (op0 << 20) | (op2 << 17) | (op1 << 14) | (crn << 10) | (crm << 1);
}
constexpr u64 CntfrqEl0 = SystemRegister(3, 3, 14, 0, 0);
constexpr u64 CntpctEl0 = SystemRegister(3, 3, 14, 0, 1);
constexpr u64 CntvctEl0 = SystemRegister(3, 3, 14, 0, 2);

void Check(hv_return_t ret) {
    ASSERT_MSG(ret == HV_SUCCESS, "Hypervisor.framework call failed: {:#x}", static_cast<u32>(ret));
}

u64 GetReg(const Hvf::Vcpu& vcpu, hv_reg_t reg) {
    u64 value{};
    Check(hv_vcpu_get_reg(vcpu.id, reg, &value));
    return value;
}

void SetReg(const Hvf::Vcpu& vcpu, hv_reg_t reg, u64 value) {
    Check(hv_vcpu_set_reg(vcpu.id, reg, value));
}

u64 GetSys(const Hvf::Vcpu& vcpu, hv_sys_reg_t reg) {
    u64 value{};
    Check(hv_vcpu_get_sys_reg(vcpu.id, reg, &value));
    return value;
}

void SetSys(const Hvf::Vcpu& vcpu, hv_sys_reg_t reg, u64 value) {
    Check(hv_vcpu_set_sys_reg(vcpu.id, reg, value));
}

hv_reg_t X(size_t index) {
    return static_cast<hv_reg_t>(HV_REG_X0 + index);
}

/// Leaves the vCPU ready to resume the EL0 state through an entry stub.
void EnterKernel(const Hvf::Vcpu& vcpu, u64 pc, u64 pstate) {
    SetSys(vcpu, HV_SYS_REG_ELR_EL1, pc);
    SetSys(vcpu, HV_SYS_REG_SPSR_EL1, pstate);
    SetReg(vcpu, HV_REG_PC, Hvf::ReturnStub);
    SetReg(vcpu, HV_REG_CPSR, PstateEl1h);
}

// Runs the faulting load or store through the emulator's memory, as Dynarmic's slow path does:
// for a page that must stay protected. False when the instruction is neither.
bool InterpretAccess(const Hvf::Vcpu& vcpu, Memory::Memory& memory) {
    std::array<u64, 31> regs;
    std::array<u128, 32> vregs;
    for (size_t i = 0; i < regs.size(); ++i) {
        regs[i] = GetReg(vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i));
    }
    for (size_t i = 0; i < vregs.size(); ++i) {
        hv_simd_fp_uchar16_t value;
        Check(hv_vcpu_get_simd_fp_reg(vcpu.id, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                      &value));
        std::memcpy(&vregs[i], &value, sizeof(value));
    }
    u64 sp = GetSys(vcpu, HV_SYS_REG_SP_EL0);
    const u64 pc = GetSys(vcpu, HV_SYS_REG_ELR_EL1);
    const std::optional<u64> next = MatchAndExecuteOneInstruction(memory, regs, vregs, sp, pc);
    if (!next) {
        return false;
    }
    for (size_t i = 0; i < regs.size(); ++i) {
        SetReg(vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), regs[i]);
    }
    for (size_t i = 0; i < vregs.size(); ++i) {
        hv_simd_fp_uchar16_t value;
        std::memcpy(&value, &vregs[i], sizeof(value));
        Check(hv_vcpu_set_simd_fp_reg(vcpu.id, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                      value));
    }
    SetSys(vcpu, HV_SYS_REG_SP_EL0, sp);
    SetSys(vcpu, HV_SYS_REG_ELR_EL1, *next);
    return true;
}

void SkipInstruction(const Hvf::Vcpu& vcpu) {
    SetSys(vcpu, HV_SYS_REG_ELR_EL1, GetSys(vcpu, HV_SYS_REG_ELR_EL1) + 4);
}

struct ThreadVcpu {
    ~ThreadVcpu() {
        if (!created) {
            return;
        }
        if (ArmHvf* const owner = vcpu.owner.load()) {
            owner->Unload(vcpu);
        }
        Hvf::VirtualMachine::Instance()->UnregisterVcpu(&vcpu);
        hv_vcpu_destroy(vcpu.id);
    }

    Hvf::Vcpu vcpu;
    bool created{};
};
thread_local ThreadVcpu t_vcpu;

Hvf::Vcpu& CurrentVcpu(Hvf::VirtualMachine& vm) {
    Hvf::Vcpu& vcpu = t_vcpu.vcpu;
    if (t_vcpu.created) {
        return vcpu;
    }
    hv_vcpu_exit_t* exit{};
    Check(hv_vcpu_create(&vcpu.id, &exit, nullptr));
    vcpu.exit = exit;
    t_vcpu.created = true;

    // 39-bit halves with 4 KiB pages, physical addresses as wide as the host's.
    const u64 pa_range = GetSys(vcpu, HV_SYS_REG_ID_AA64MMFR0_EL1) & 0x7;
    const u64 t0sz = 64 - Hvf::AddressSpaceBits;
    const u64 tcr = t0sz | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (t0sz << 16) |
                    (1ULL << 24) | (1ULL << 26) | (3ULL << 28) | (2ULL << 30) | (pa_range << 32);
    // MMU and caches on; EL0 may use DC ZVA, CTR_EL0, cache maintenance and WFI/WFE.
    const u64 sctlr = (1ULL << 0) | (1ULL << 2) | (1ULL << 12) | (1ULL << 14) | (1ULL << 15) |
                      (1ULL << 16) | (1ULL << 18) | (1ULL << 26) |
                      // RES1
                      (1ULL << 11) | (1ULL << 20) | (1ULL << 22) | (1ULL << 23) | (1ULL << 28) |
                      (1ULL << 29);
    SetSys(vcpu, HV_SYS_REG_MAIR_EL1, 0xff);
    SetSys(vcpu, HV_SYS_REG_TCR_EL1, tcr);
    SetSys(vcpu, HV_SYS_REG_TTBR1_EL1, vm.KernelTable());
    SetSys(vcpu, HV_SYS_REG_VBAR_EL1, Hvf::KernelBase);
    // No FP/SIMD traps.
    SetSys(vcpu, HV_SYS_REG_CPACR_EL1, 3ULL << 20);
    // EL0 reads of the counters trap: the guest's run at another frequency.
    SetSys(vcpu, HV_SYS_REG_CNTKCTL_EL1, 0);
    SetSys(vcpu, HV_SYS_REG_SCTLR_EL1, sctlr);
    Check(hv_vcpu_set_trap_debug_exceptions(vcpu.id, false));
    Check(hv_vcpu_set_vtimer_mask(vcpu.id, true));

    vm.RegisterVcpu(&vcpu);
    return vcpu;
}

} // namespace

bool ArmHvf::IsUsable(const Kernel::KProcess& process, u32 address_space_bits) {
    if (Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::Hvf ||
        !process.IsApplication()) {
        return false;
    }
    if (address_space_bits <= 32 || address_space_bits > Hvf::AddressSpaceBits) {
        LOG_WARNING(Core_ARM, "The hypervisor runs 64-bit programs only, falling back to Dynarmic");
        return false;
    }
    if (!Settings::IsFastmemEnabled()) {
        LOG_WARNING(Core_ARM, "The hypervisor needs fastmem, falling back to Dynarmic");
        return false;
    }
    if (Hvf::VirtualMachine::Instance() == nullptr) {
        LOG_WARNING(Core_ARM, "The hypervisor is unavailable, falling back to Dynarmic");
        return false;
    }
    LOG_INFO(Core_ARM, "CPU backend: Hypervisor.framework");
    return true;
}

ArmHvf::ArmHvf(System& system, Kernel::KProcess* process, std::size_t core_index)
    : ArmInterface{true}, m_system{system}, m_memory{process->GetMemory()},
      m_vm{*Hvf::VirtualMachine::Instance()}, m_space{*m_memory.GetHvfAddressSpace()},
      m_process{process},
      m_core_index{core_index} {}

ArmHvf::~ArmHvf() {
    if (m_live != nullptr) {
        m_live->owner.store(nullptr);
    }
}

Hvf::Vcpu* ArmHvf::LiveVcpu() const {
    if (m_live == nullptr) {
        return nullptr;
    }
    if (m_live != &t_vcpu.vcpu) {
        // A vCPU's registers can only be read from its thread.
        LOG_ERROR(Core_ARM, "Core {} context accessed from another thread while a vCPU holds it",
                  m_core_index);
        return nullptr;
    }
    return m_live;
}

void ArmHvf::Push(Hvf::Vcpu& vcpu) {
    for (size_t i = 0; i < 29; ++i) {
        SetReg(vcpu, X(i), m_ctx.r[i]);
    }
    SetReg(vcpu, HV_REG_FP, m_ctx.fp);
    SetReg(vcpu, HV_REG_LR, m_ctx.lr);
    SetSys(vcpu, HV_SYS_REG_SP_EL0, m_ctx.sp);
    for (size_t i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t value;
        std::memcpy(&value, &m_ctx.v[i], sizeof(value));
        Check(hv_vcpu_set_simd_fp_reg(vcpu.id, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                      value));
    }
    SetReg(vcpu, HV_REG_FPCR, m_ctx.fpcr);
    SetReg(vcpu, HV_REG_FPSR, m_ctx.fpsr);
    SetSys(vcpu, HV_SYS_REG_TPIDR_EL0, m_ctx.tpidr);
    SetSys(vcpu, HV_SYS_REG_TPIDRRO_EL0, m_tpidrro_el0);
    SetSys(vcpu, HV_SYS_REG_TTBR0_EL1, m_space.Ttbr0());
    EnterKernel(vcpu, m_ctx.pc, m_ctx.pstate & PstateFlags);
}

void ArmHvf::Pull(Hvf::Vcpu& vcpu) const {
    for (size_t i = 0; i < 29; ++i) {
        m_ctx.r[i] = GetReg(vcpu, X(i));
    }
    m_ctx.fp = GetReg(vcpu, HV_REG_FP);
    m_ctx.lr = GetReg(vcpu, HV_REG_LR);
    m_ctx.sp = GetSys(vcpu, HV_SYS_REG_SP_EL0);
    // Outside RunThread the vCPU always waits in the kernel page.
    m_ctx.pc = GetSys(vcpu, HV_SYS_REG_ELR_EL1);
    m_ctx.pstate = static_cast<u32>(GetSys(vcpu, HV_SYS_REG_SPSR_EL1));
    for (size_t i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t value;
        Check(hv_vcpu_get_simd_fp_reg(vcpu.id, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                      &value));
        std::memcpy(&m_ctx.v[i], &value, sizeof(value));
    }
    m_ctx.fpcr = static_cast<u32>(GetReg(vcpu, HV_REG_FPCR));
    m_ctx.fpsr = static_cast<u32>(GetReg(vcpu, HV_REG_FPSR));
    m_ctx.tpidr = GetSys(vcpu, HV_SYS_REG_TPIDR_EL0);
}

void ArmHvf::Load(Hvf::Vcpu& vcpu) {
    if (m_live == &vcpu) {
        return;
    }
    ASSERT_MSG(m_live == nullptr, "Core {} context is held by another thread's vCPU", m_core_index);
    if (ArmHvf* const owner = vcpu.owner.load()) {
        owner->Unload(vcpu);
    }
    Push(vcpu);
    vcpu.owner.store(this);
    m_live = &vcpu;
}

void ArmHvf::Unload(Hvf::Vcpu& vcpu) {
    Pull(vcpu);
    vcpu.owner.store(nullptr);
    m_live = nullptr;
}

HaltReason ArmHvf::RunThread(Kernel::KThread* thread) {
    // Check if we're already interrupted.
    if (const auto hr = static_cast<HaltReason>(m_halt.exchange(0)); True(hr)) {
        return hr;
    }

    Hvf::Vcpu& vcpu = CurrentVcpu(m_vm);
    Load(vcpu);
    m_running.store(&vcpu);

    HaltReason hr{};
    while (true) {
        hr = static_cast<HaltReason>(m_halt.exchange(0));
        if (True(hr)) {
            break;
        }
        vcpu.in_guest.store(true);
        if (const u64 generation = m_vm.TlbGeneration();
            generation != vcpu.seen_tlb_generation.load(std::memory_order_relaxed)) {
            SetReg(vcpu, HV_REG_PC, Hvf::FlushStub);
            vcpu.seen_tlb_generation.store(generation);
        }
        Check(hv_vcpu_run(vcpu.id));
        vcpu.in_guest.store(false);

        hr = HandleExit(vcpu);
        if (True(hr)) {
            break;
        }
    }

    m_running.store(nullptr);
    return hr;
}

HaltReason ArmHvf::HandleExit(Hvf::Vcpu& vcpu) {
    const auto* const exit = static_cast<const hv_vcpu_exit_t*>(vcpu.exit);
    switch (exit->reason) {
    case HV_EXIT_REASON_CANCELED: {
        const u64 pc = GetReg(vcpu, HV_REG_PC);
        const u64 pstate = GetReg(vcpu, HV_REG_CPSR);
        if ((pstate & PstateModeMask) == 0) {
            EnterKernel(vcpu, pc, pstate);
            return {};
        }
        // Kicked out between taking an exception and the vector's hypervisor call: the exception
        // would be lost if the context were saved and loaded again, so handle it now.
        const u64 offset = pc - Hvf::KernelBase;
        if (offset < Hvf::VectorSize * Hvf::VectorCount && offset % Hvf::VectorSize == 0) {
            SetReg(vcpu, HV_REG_PC, pc + 4);
            return HandleVector(vcpu, offset / Hvf::VectorSize);
        }
        return {};
    }
    case HV_EXIT_REASON_EXCEPTION: {
        const u64 syndrome = exit->exception.syndrome;
        const u64 ec = syndrome >> 26;
        if (ec == EcHvc) {
            return HandleVector(vcpu, syndrome & 0xffff);
        }
        const u64 pc = GetReg(vcpu, HV_REG_PC);
        const u64 pstate = GetReg(vcpu, HV_REG_CPSR);
        if ((pstate & PstateModeMask) != 0) {
            LOG_CRITICAL(Core_ARM, "Hypervisor exception in the guest kernel: syndrome {:#x}, pc "
                                   "{:#x}, address {:#x}",
                         syndrome, pc, exit->exception.virtual_address);
            return HaltReason::PrefetchAbort;
        }
        if (ec == EcWfx) {
            // The host traps WFI; there are no guest interrupts to wait for.
            EnterKernel(vcpu, pc + 4, pstate);
            return {};
        }
        // Stage 2 faults and the like: the guest reached memory that is not its own.
        LOG_CRITICAL(Core_ARM, "Unhandled hypervisor exception: syndrome {:#x}, pc {:#x}, address "
                               "{:#x}, physical {:#x}",
                     syndrome, pc, exit->exception.virtual_address,
                     exit->exception.physical_address);
        EnterKernel(vcpu, pc, pstate);
        return HaltReason::PrefetchAbort;
    }
    case HV_EXIT_REASON_VTIMER_ACTIVATED:
        return {};
    default:
        LOG_ERROR(Core_ARM, "Unknown hypervisor exit reason {}", static_cast<u32>(exit->reason));
        return {};
    }
}

HaltReason ArmHvf::HandleVector(Hvf::Vcpu& vcpu, u64 vector) {
    const u64 syndrome = GetSys(vcpu, HV_SYS_REG_ESR_EL1);
    if (vector != Hvf::LowerElSyncVector) {
        LOG_CRITICAL(Core_ARM, "Unexpected guest exception: vector {}, syndrome {:#x}, pc {:#x}, "
                               "address {:#x}",
                     vector, syndrome, GetSys(vcpu, HV_SYS_REG_ELR_EL1),
                     GetSys(vcpu, HV_SYS_REG_FAR_EL1));
        return HaltReason::PrefetchAbort;
    }

    switch (syndrome >> 26) {
    case EcSvc:
        m_svc = static_cast<u32>(syndrome & 0xffff);
        return HaltReason::SupervisorCall;
    case EcDataAbort:
        return HandleDataAbort(vcpu, syndrome);
    case EcSystemRegister:
        HandleSystemRegister(vcpu, syndrome);
        return {};
    case EcInstructionAbort: {
        const u64 address = GetSys(vcpu, HV_SYS_REG_FAR_EL1);
        if (m_space.Query(address, false) == Hvf::AddressSpace::Access::Allowed) {
            // The TLB held a translation older than the tables.
            SetReg(vcpu, HV_REG_PC, Hvf::FlushStub);
            return {};
        }
        LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", address);
        return HaltReason::PrefetchAbort;
    }
    case EcBreakpoint:
        return HaltReason::InstructionBreakpoint;
    default: {
        const u64 pc = GetSys(vcpu, HV_SYS_REG_ELR_EL1);
        LOG_CRITICAL(Core_ARM, "Unhandled guest exception: syndrome {:#x}, pc {:#x}, code {:08X}",
                     syndrome, pc, m_memory.Read32(pc));
        return HaltReason::PrefetchAbort;
    }
    }
}

HaltReason ArmHvf::HandleDataAbort(Hvf::Vcpu& vcpu, u64 syndrome) {
    using Access = Hvf::AddressSpace::Access;
    const u64 address = GetSys(vcpu, HV_SYS_REG_FAR_EL1);
    const u64 pc = GetSys(vcpu, HV_SYS_REG_ELR_EL1);
    const bool write = (syndrome & (1ULL << 6)) != 0;

    // Whatever let the access through may have added permissions to a cached translation.
    const auto retry = [&] {
        SetReg(vcpu, HV_REG_PC, Hvf::FlushStub);
        return HaltReason{};
    };

    const Access access = m_space.Query(address, write);
    if (access == Access::Allowed) {
        return retry();
    }

    if (pc == m_fault_pc && address == m_fault_address) {
        ++m_fault_repeats;
    } else {
        m_fault_pc = pc;
        m_fault_address = address;
        m_fault_repeats = 0;
    }

    auto& page_table = m_process->GetPageTable().GetBasePageTable();
    const u64 page_address = address & ~Memory::YUZU_PAGEMASK;
    if (access == Access::Denied &&
        std::as_const(page_table.GetImpl().entries)[address >> Memory::YUZU_PAGEBITS].Type() ==
            Common::PageType::Memory) {
        // The GPU no longer tracks the page, yet it kept a protection (a read trap disarmed after
        // the page left the cache leaves it read-only). Dynarmic's slow path accesses such pages
        // freely; give the page the permissions the kernel has for it.
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page_info{};
        if (page_table.QueryInfo(&info, &page_info, address).IsSuccess()) {
            Common::MemoryPermission perms{};
            if (True(info.GetPermission() & Kernel::KMemoryPermission::UserRead)) {
                perms |= Common::MemoryPermission::Read;
            }
            if (True(info.GetPermission() & Kernel::KMemoryPermission::UserWrite)) {
                perms |= Common::MemoryPermission::Write;
            }
            if (True(perms & (write ? Common::MemoryPermission::Write
                                    : Common::MemoryPermission::Read))) {
                m_space.Protect(page_address, Memory::YUZU_PAGESIZE, perms);
                return retry();
            }
        }
    }

    // Pages are protected while the GPU caches them, as with fastmem.
    if (access == Access::Denied && m_fault_repeats < 3) {
        if (!write) {
            // The slow read path brings the GPU's data and disarms a read trap, unless it finds
            // the data current already: the page is as readable then, and only its trap is left.
            (void)m_memory.Read8(address);
            if (m_space.Query(address, false) == Access::Allowed ||
                m_memory.GrantCachedPageRead(address)) {
                return retry();
            }
        }
        if (m_memory.InvalidateNCE(page_address, Memory::YUZU_PAGESIZE) &&
            m_space.Query(address, write) == Access::Allowed) {
            return retry();
        }
    }
    // The page stays protected (the GPU still tracks it): the access goes through the emulator's
    // memory instead.
    if (access == Access::Denied &&
        std::as_const(page_table.GetImpl().entries)[address >> Memory::YUZU_PAGEBITS].Type() ==
            Common::PageType::RasterizerCachedMemory &&
        InterpretAccess(vcpu, m_memory)) {
        m_fault_repeats = 0;
        return {};
    }

    // Skip the instruction: this lets games continue where they would otherwise crash.
    static std::atomic<u64> skipped{};
    if (const u64 count = skipped.fetch_add(1); count < 100 || count % 100000 == 0) {
        // Why it was skipped: what the guest's tables, the emulator's page table and the kernel
        // each say about the page, and how many times the fault was resolved without effect.
        static constexpr std::array page_types{"unmapped", "memory", "debug", "gpu-cached"};
        const Access readable = m_space.Query(address, false);
        const char* const guest_tables = access == Access::Unmapped  ? "absent"
                                         : readable == Access::Allowed ? "read-only"
                                                                       : "no access";
        const bool in_range = address < (1ULL << Hvf::AddressSpaceBits);
        const auto page_type = in_range ? std::as_const(page_table.GetImpl().entries)
                                              [address >> Memory::YUZU_PAGEBITS].Type()
                                        : Common::PageType::Unmapped;
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page_info{};
        const bool queried = page_table.QueryInfo(&info, &page_info, address).IsSuccess();
        LOG_ERROR(Core_ARM,
                  "Invalid {} of {:#016x} at pc {:#x} skipped (#{}): guest tables {}, page type "
                  "{}, kernel state {:#x} permission {:#x} attribute {:#x}, {} retries",
                  write ? "write" : "read", address, pc, count, guest_tables,
                  page_types[static_cast<size_t>(page_type)],
                  queried ? static_cast<u32>(info.GetSvcState()) : ~0u,
                  queried ? static_cast<u32>(info.GetPermission()) : ~0u,
                  queried ? static_cast<u32>(info.GetAttribute()) : ~0u, m_fault_repeats);
    }
    m_fault_repeats = 0;
    SkipInstruction(vcpu);
    return {};
}

void ArmHvf::HandleSystemRegister(Hvf::Vcpu& vcpu, u64 syndrome) {
    const bool read = (syndrome & 1) != 0;
    const size_t rt = (syndrome >> 5) & 31;
    u64 value = 0;
    switch (read ? (syndrome & SystemRegisterMask) : ~0ULL) {
    case CntpctEl0:
    case CntvctEl0:
        value = m_system.CoreTiming().GetClockTicks();
        break;
    case CntfrqEl0:
        value = Common::g_wall_clock.GetGuestCNTFRQ();
        break;
    default:
        LOG_ERROR(Core_ARM, "Unhandled system register access at pc {:#x}: syndrome {:#x}",
                  GetSys(vcpu, HV_SYS_REG_ELR_EL1), syndrome);
        break;
    }
    if (read && rt != 31) {
        SetReg(vcpu, X(rt), value);
    }
    SkipInstruction(vcpu);
}

HaltReason ArmHvf::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

u32 ArmHvf::GetSvcNumber() const {
    return m_svc;
}

void ArmHvf::GetSvcArguments(std::span<uint64_t, 8> args) const {
    const Hvf::Vcpu* const vcpu = LiveVcpu();
    for (size_t i = 0; i < 8; ++i) {
        args[i] = vcpu ? GetReg(*vcpu, X(i)) : m_ctx.r[i];
    }
}

void ArmHvf::SetSvcArguments(std::span<const uint64_t, 8> args) {
    const Hvf::Vcpu* const vcpu = LiveVcpu();
    for (size_t i = 0; i < 8; ++i) {
        if (vcpu) {
            SetReg(*vcpu, X(i), args[i]);
        } else {
            m_ctx.r[i] = args[i];
        }
    }
}

void ArmHvf::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    if (Hvf::Vcpu* const vcpu = LiveVcpu()) {
        Pull(*vcpu);
    }
    ctx = m_ctx;
}

void ArmHvf::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    if (m_live != nullptr) {
        m_live->owner.store(nullptr);
        m_live = nullptr;
    }
    m_ctx = ctx;
}

void ArmHvf::SetTpidrroEl0(u64 value) {
    m_tpidrro_el0 = value;
    if (const Hvf::Vcpu* const vcpu = LiveVcpu()) {
        SetSys(*vcpu, HV_SYS_REG_TPIDRRO_EL0, value);
    }
}

void ArmHvf::SignalInterrupt(Kernel::KThread* thread) {
    m_halt.fetch_or(static_cast<u64>(HaltReason::BreakLoop));
    // The request stays pending when it arrives before the vCPU enters the guest.
    if (const Hvf::Vcpu* const vcpu = m_running.load()) {
        hv_vcpu_t id = vcpu->id;
        hv_vcpus_exit(&id, 1);
    }
}

void ArmHvf::ClearInstructionCache() {}

void ArmHvf::InvalidateCacheRange(u64 addr, std::size_t size) {
    // The guest runs the code where the host wrote it: only the host's instruction cache needs
    // to hear about it, through any mapping of the pages.
    const u64 end = addr + size;
    while (addr < end) {
        const u64 next = (std::min)(end, (addr & ~Memory::YUZU_PAGEMASK) + Memory::YUZU_PAGESIZE);
        if (u8* const pointer = m_memory.GetPointerSilent(addr)) {
            sys_icache_invalidate(pointer, next - addr);
        }
        addr = next;
    }
}

} // namespace Core
