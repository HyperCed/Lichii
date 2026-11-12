// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "core/arm/arm_interface.h"
#include "core/hle/kernel/svc_types.h"

namespace Core::Memory {
class Memory;
}

namespace Core::Hvf {
class AddressSpace;
class VirtualMachine;
struct Vcpu;
} // namespace Core::Hvf

namespace Core {

class System;

/// Runs guest code on the host CPU with Hypervisor.framework (Apple Silicon). Each host thread
/// that runs guest code owns one vCPU; the interfaces of that core share it.
class ArmHvf final : public ArmInterface {
public:
    ArmHvf(System& system, Kernel::KProcess* process, std::size_t core_index);
    ~ArmHvf() override;

    /// Whether the process's code can run under the hypervisor with the current settings.
    static bool IsUsable(const Kernel::KProcess& process, u32 address_space_bits);

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    /// Takes the guest context back from a vCPU that is going away or changing owner.
    void Unload(Hvf::Vcpu& vcpu);

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }

    void RewindBreakpointInstruction() override {}

private:
    void Load(Hvf::Vcpu& vcpu);
    void Push(Hvf::Vcpu& vcpu);
    void Pull(Hvf::Vcpu& vcpu) const;
    /// The vCPU holding the context when the calling thread may read it, else null.
    Hvf::Vcpu* LiveVcpu() const;

    HaltReason HandleExit(Hvf::Vcpu& vcpu);
    HaltReason HandleVector(Hvf::Vcpu& vcpu, u64 vector);
    HaltReason HandleDataAbort(Hvf::Vcpu& vcpu, u64 syndrome);
    void HandleSystemRegister(Hvf::Vcpu& vcpu, u64 syndrome);

    System& m_system;
    Memory::Memory& m_memory;
    Hvf::VirtualMachine& m_vm;
    Hvf::AddressSpace& m_space;
    Kernel::KProcess* m_process{};
    std::size_t m_core_index{};

    // The guest context while no vCPU holds it (m_live is null); a snapshot otherwise.
    mutable Kernel::Svc::ThreadContext m_ctx{};
    u64 m_tpidrro_el0{};
    mutable Hvf::Vcpu* m_live{};
    u32 m_svc{};

    std::atomic<Hvf::Vcpu*> m_running{};
    std::atomic<u64> m_halt{};

    // A fault that repeats without progress is skipped rather than retried forever.
    u64 m_fault_pc{};
    u64 m_fault_address{};
    u32 m_fault_repeats{};
};

} // namespace Core
