// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "common/host_memory.h"

namespace Core {
class ArmHvf;
}

namespace Core::Hvf {

// The guest runs its code at EL0 under a one-page EL1 "kernel" that lives in the upper half of
// the address space. Every exception EL0 takes lands on a vector that leaves the guest with a
// hypervisor call, and the guest is always entered through one of the two stubs below, which
// return to EL0 with the state held in ELR_EL1 and SPSR_EL1.
constexpr u64 KernelBase = 0xFFFF'FF80'0000'0000ULL;
constexpr u64 VectorSize = 0x80;
constexpr u64 VectorCount = 16;
constexpr u64 ReturnStub = KernelBase + 0x800;
/// Flushes this vCPU's TLB before returning.
constexpr u64 FlushStub = KernelBase + 0x810;
/// Vector taken by synchronous exceptions from AArch64 EL0.
constexpr u64 LowerElSyncVector = 8;

constexpr u32 AddressSpaceBits = 39;

struct Vcpu {
    u64 id{};
    void* exit{};
    /// Set around hv_vcpu_run, with TLB shootdowns in mind.
    std::atomic<bool> in_guest{};
    std::atomic<u64> seen_tlb_generation{};
    /// The interface whose guest context the vCPU's registers hold.
    std::atomic<ArmHvf*> owner{};
};

/// The process's only Hypervisor.framework VM (the framework allows one per process).
class VirtualMachine {
public:
    /// Null when the VM can't be created (no com.apple.security.hypervisor entitlement).
    static VirtualMachine* Instance();

    /// Makes the emulated physical memory visible to the guest.
    bool SetBacking(u8* base, size_t size);
    u64 BackingAddress(u64 offset) const {
        return BackingBase + offset;
    }

    /// Translation table pages, zeroed, by guest physical address.
    u64 AllocateTable();
    void FreeTable(u64 address);
    u64* TablePointer(u64 address) const {
        return reinterpret_cast<u64*>(m_pool + (address - PoolBase));
    }

    u64 AllocateAsid();
    u64 KernelTable() const {
        return m_kernel_table;
    }

    void RegisterVcpu(Vcpu* vcpu);
    void UnregisterVcpu(Vcpu* vcpu);

    u64 TlbGeneration() const {
        return m_tlb_generation.load();
    }
    /// Every vCPU flushes its TLB before it next runs guest code. Use it where no vCPU can be
    /// running with the stale translations.
    void InvalidateTlbLazily() {
        m_tlb_generation.fetch_add(1);
    }
    /// As above, and returns once no vCPU is running guest code with stale translations.
    void InvalidateTlb();

private:
    VirtualMachine() = default;
    bool Initialize();

    static constexpr u64 PoolBase = 0x1'0000ULL;
    static constexpr u64 PoolSize = 1ULL << 30;
    static constexpr u64 BackingBase = 1ULL << 32;

    u8* m_pool{};
    u64 m_pool_next{};
    std::vector<u64> m_free_tables;
    std::mutex m_pool_mutex;

    u64 m_kernel_table{};
    u8* m_backing{};
    size_t m_backing_size{};
    u32 m_next_asid{};

    std::mutex m_vcpu_mutex;
    std::vector<Vcpu*> m_vcpus;
    std::atomic<u64> m_tlb_generation{1};
};

/// The guest's stage 1 translation tables for one process: what its fastmem arena maps, with 4 KiB
/// pages whatever the host's page size.
class AddressSpace {
public:
    explicit AddressSpace(VirtualMachine& vm);
    ~AddressSpace();

    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    void Map(u64 vaddr, u64 backing_offset, u64 size, Common::MemoryPermission perms);
    void Unmap(u64 vaddr, u64 size);
    void Protect(u64 vaddr, u64 size, Common::MemoryPermission perms);

    enum class Access { Unmapped, Denied, Allowed };
    Access Query(u64 vaddr, bool write) const;

    u64 Ttbr0() const {
        return m_root | (m_asid << 48);
    }

private:
    /// The level 3 table covering vaddr's 2 MiB, or null.
    u64* LeafTable(u64 vaddr, bool allocate);
    /// Applies update to each mapped page's descriptor and shoots the TLBs down when one lost
    /// access.
    template <typename Func>
    void Update(u64 vaddr, u64 size, bool allocate, Func&& update);

    VirtualMachine& m_vm;
    u64 m_root{};
    u64 m_asid{};
    std::vector<u64> m_tables;
    std::mutex m_mutex;
};

} // namespace Core::Hvf
