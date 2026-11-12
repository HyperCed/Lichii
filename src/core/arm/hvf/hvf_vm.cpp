// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <thread>

#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>

#include "common/assert.h"
#include "common/logging.h"
#include "core/arm/hvf/hvf_vm.h"

namespace Core::Hvf {

namespace {

constexpr u64 PageSize = 0x1000;
constexpr u64 LeafSpan = 1ULL << 21;
constexpr u64 EntriesPerTable = 512;

constexpr u64 DescValid = 1ULL << 0;
constexpr u64 DescTable = 3;
constexpr u64 DescPage = 3;
constexpr u64 DescReadOnly = 1ULL << 7;
constexpr u64 DescUser = 1ULL << 6;
constexpr u64 DescInnerShareable = 3ULL << 8;
constexpr u64 DescAccessed = 1ULL << 10;
constexpr u64 DescNotGlobal = 1ULL << 11;
constexpr u64 DescPxn = 1ULL << 53;
constexpr u64 DescUxn = 1ULL << 54;
constexpr u64 DescAddressMask = 0x0000'FFFF'FFFF'F000ULL;

// A page the guest may not access keeps its descriptor with the valid bit clear.
constexpr u64 DescPermissionMask = DescValid | DescReadOnly;

u64 PermissionBits(Common::MemoryPermission perms) {
    if (True(perms & Common::MemoryPermission::Write)) {
        return DescValid;
    }
    if (True(perms & Common::MemoryPermission::Read)) {
        return DescValid | DescReadOnly;
    }
    return 0;
}

u64 Load(const u64* entry) {
    return __atomic_load_n(entry, __ATOMIC_RELAXED);
}

void Store(u64* entry, u64 value) {
    __atomic_store_n(entry, value, __ATOMIC_RELEASE);
}

constexpr u32 Hvc(u32 imm) {
    return 0xD4000002 | (imm << 5);
}
constexpr u32 Eret = 0xD69F03E0;
constexpr u32 TlbiVmalle1 = 0xD508871F;
constexpr u32 DsbNsh = 0xD503379F;
constexpr u32 Isb = 0xD5033FDF;

} // namespace

VirtualMachine* VirtualMachine::Instance() {
    static VirtualMachine* const instance = []() -> VirtualMachine* {
        auto* vm = new VirtualMachine;
        if (!vm->Initialize()) {
            delete vm;
            return nullptr;
        }
        return vm;
    }();
    return instance;
}

bool VirtualMachine::Initialize() {
    if (const hv_return_t ret = hv_vm_create(nullptr); ret != HV_SUCCESS) {
        LOG_ERROR(Core_ARM,
                  "Hypervisor.framework refused to create a VM ({:#x}); the program needs the "
                  "com.apple.security.hypervisor entitlement",
                  static_cast<u32>(ret));
        return false;
    }
    void* const pool = mmap(nullptr, PoolSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (pool == MAP_FAILED) {
        LOG_ERROR(Core_ARM, "Could not reserve the guest translation tables");
        return false;
    }
    m_pool = static_cast<u8*>(pool);
    if (const hv_return_t ret = hv_vm_map(m_pool, PoolBase, PoolSize,
                                          HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
        ret != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "hv_vm_map of the translation tables failed ({:#x})",
                  static_cast<u32>(ret));
        return false;
    }

    // The kernel page: vectors and entry stubs.
    const u64 code_page = AllocateTable();
    auto* const code = reinterpret_cast<u32*>(TablePointer(code_page));
    for (u32 i = 0; i < VectorCount; ++i) {
        code[i * VectorSize / 4] = Hvc(i);
        code[i * VectorSize / 4 + 1] = Eret;
    }
    code[(ReturnStub - KernelBase) / 4] = Eret;
    u32* const flush = code + (FlushStub - KernelBase) / 4;
    flush[0] = TlbiVmalle1;
    flush[1] = DsbNsh;
    flush[2] = Isb;
    flush[3] = Eret;

    m_kernel_table = AllocateTable();
    const u64 level2 = AllocateTable();
    const u64 level3 = AllocateTable();
    TablePointer(m_kernel_table)[0] = level2 | DescTable;
    TablePointer(level2)[0] = level3 | DescTable;
    TablePointer(level3)[0] =
        code_page | DescPage | DescAccessed | DescInnerShareable | DescReadOnly | DescUxn;
    return true;
}

bool VirtualMachine::SetBacking(u8* base, size_t size) {
    std::scoped_lock lk{m_pool_mutex};
    if (m_backing == base && m_backing_size == size) {
        return true;
    }
    if (m_backing != nullptr) {
        hv_vm_unmap(BackingBase, m_backing_size);
        m_backing = nullptr;
    }
    if (const hv_return_t ret = hv_vm_map(base, BackingBase, size,
                                          HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
        ret != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "hv_vm_map of {} MiB of guest memory failed ({:#x})", size >> 20,
                  static_cast<u32>(ret));
        return false;
    }
    m_backing = base;
    m_backing_size = size;
    return true;
}

u64 VirtualMachine::AllocateTable() {
    std::scoped_lock lk{m_pool_mutex};
    u64 address;
    if (!m_free_tables.empty()) {
        address = m_free_tables.back();
        m_free_tables.pop_back();
    } else {
        ASSERT_MSG(m_pool_next < PoolSize, "Out of guest translation tables");
        address = PoolBase + m_pool_next;
        m_pool_next += PageSize;
    }
    std::memset(TablePointer(address), 0, PageSize);
    return address;
}

void VirtualMachine::FreeTable(u64 address) {
    std::scoped_lock lk{m_pool_mutex};
    m_free_tables.push_back(address);
}

u64 VirtualMachine::AllocateAsid() {
    std::scoped_lock lk{m_pool_mutex};
    // An ASID may come back with translations of its previous user still cached.
    InvalidateTlbLazily();
    return 1 + (m_next_asid++ % 255);
}

void VirtualMachine::RegisterVcpu(Vcpu* vcpu) {
    std::scoped_lock lk{m_vcpu_mutex};
    m_vcpus.push_back(vcpu);
}

void VirtualMachine::UnregisterVcpu(Vcpu* vcpu) {
    std::scoped_lock lk{m_vcpu_mutex};
    std::erase(m_vcpus, vcpu);
}

void VirtualMachine::InvalidateTlb() {
    const u64 generation = m_tlb_generation.fetch_add(1) + 1;
    std::scoped_lock lk{m_vcpu_mutex};
    for (Vcpu* const vcpu : m_vcpus) {
        // A vCPU that isn't in the guest reads the generation before it enters. One that is gets
        // kicked out: the request stays pending if it arrives before hv_vcpu_run.
        for (u32 spins = 0;
             vcpu->in_guest.load() && vcpu->seen_tlb_generation.load() < generation; ++spins) {
            if ((spins & 0x3ff) == 0) {
                hv_vcpu_t id = vcpu->id;
                hv_vcpus_exit(&id, 1);
            }
            std::this_thread::yield();
        }
    }
}

AddressSpace::AddressSpace(VirtualMachine& vm)
    : m_vm{vm}, m_root{vm.AllocateTable()}, m_asid{vm.AllocateAsid()} {
    m_tables.push_back(m_root);
}

AddressSpace::~AddressSpace() {
    for (const u64 table : m_tables) {
        m_vm.FreeTable(table);
    }
}

u64* AddressSpace::LeafTable(u64 vaddr, bool allocate) {
    u64* table = m_vm.TablePointer(m_root);
    for (const u32 shift : {30u, 21u}) {
        u64* const entry = table + ((vaddr >> shift) & (EntriesPerTable - 1));
        u64 value = Load(entry);
        if (value == 0) {
            if (!allocate) {
                return nullptr;
            }
            value = m_vm.AllocateTable() | DescTable;
            m_tables.push_back(value & DescAddressMask);
            Store(entry, value);
        }
        table = m_vm.TablePointer(value & DescAddressMask);
    }
    return table;
}

template <typename Func>
void AddressSpace::Update(u64 vaddr, u64 size, bool allocate, Func&& update) {
    if (vaddr >= (1ULL << AddressSpaceBits)) {
        return;
    }
    const u64 end = (std::min)(vaddr + size, 1ULL << AddressSpaceBits);
    bool lost_access = false;
    {
        std::scoped_lock lk{m_mutex};
        while (vaddr < end) {
            const u64 chunk_end = (std::min)(end, (vaddr & ~(LeafSpan - 1)) + LeafSpan);
            u64* const table = LeafTable(vaddr, allocate);
            if (table == nullptr) {
                vaddr = chunk_end;
                continue;
            }
            for (; vaddr < chunk_end; vaddr += PageSize) {
                u64* const entry = table + ((vaddr >> 12) & (EntriesPerTable - 1));
                const u64 old_value = Load(entry);
                const u64 new_value = update(vaddr, old_value);
                if (new_value == old_value) {
                    continue;
                }
                Store(entry, new_value);
                // Translations that fault are not cached.
                if ((old_value & DescValid) != 0 &&
                    ((new_value & DescValid) == 0 ||
                     (new_value & DescAddressMask) != (old_value & DescAddressMask) ||
                     ((new_value & DescReadOnly) != 0 && (old_value & DescReadOnly) == 0))) {
                    lost_access = true;
                }
            }
        }
    }
    if (lost_access) {
        m_vm.InvalidateTlb();
    }
}

void AddressSpace::Map(u64 vaddr, u64 backing_offset, u64 size, Common::MemoryPermission perms) {
    const u64 attributes = (DescPage & ~DescValid) | DescAccessed | DescInnerShareable |
                           DescNotGlobal | DescUser | DescPxn | PermissionBits(perms);
    const u64 delta = m_vm.BackingAddress(backing_offset) - vaddr;
    Update(vaddr, size, true, [&](u64 address, u64) { return (address + delta) | attributes; });
}

void AddressSpace::Unmap(u64 vaddr, u64 size) {
    Update(vaddr, size, false, [](u64, u64) { return u64{}; });
}

void AddressSpace::Protect(u64 vaddr, u64 size, Common::MemoryPermission perms) {
    const u64 bits = PermissionBits(perms);
    Update(vaddr, size, false, [bits](u64, u64 value) {
        return value == 0 ? value : (value & ~DescPermissionMask) | bits;
    });
}

AddressSpace::Access AddressSpace::Query(u64 vaddr, bool write) const {
    if (vaddr >= (1ULL << AddressSpaceBits)) {
        return Access::Unmapped;
    }
    const u64* table = m_vm.TablePointer(m_root);
    for (const u32 shift : {30u, 21u}) {
        const u64 value = Load(table + ((vaddr >> shift) & (EntriesPerTable - 1)));
        if (value == 0) {
            return Access::Unmapped;
        }
        table = m_vm.TablePointer(value & DescAddressMask);
    }
    const u64 value = Load(table + ((vaddr >> 12) & (EntriesPerTable - 1)));
    if (value == 0) {
        return Access::Unmapped;
    }
    if ((value & DescValid) == 0 || (write && (value & DescReadOnly) != 0)) {
        return Access::Denied;
    }
    return Access::Allowed;
}

} // namespace Core::Hvf
