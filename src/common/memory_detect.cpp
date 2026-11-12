// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#ifdef __linux__
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#endif

#include "common/memory_detect.h"

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    GlobalMemoryStatusEx(&memorystatus);
    mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
    mem_info.TotalSwapMemory = memorystatus.ullTotalPageFile - mem_info.TotalPhysicalMemory;
#elif defined(__APPLE__)
    u64 ramsize;
    struct xsw_usage vmusage;
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0);
    sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0);
    mem_info.TotalPhysicalMemory = ramsize;
    mem_info.TotalSwapMemory = vmusage.xsu_total;
#elif defined(__FreeBSD__)
    u_long physmem, swap_total;
    std::size_t sizeof_u_long = sizeof(u_long);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    sysctlbyname("hw.physmem", &physmem, &sizeof_u_long, nullptr, 0);
    sysctlbyname("vm.swap_total", &swap_total, &sizeof_u_long, nullptr, 0);
    mem_info.TotalPhysicalMemory = physmem;
    mem_info.TotalSwapMemory = swap_total;
#elif defined(__linux__)
    struct sysinfo meminfo;
    sysinfo(&meminfo);
    mem_info.TotalPhysicalMemory = meminfo.totalram;
    mem_info.TotalSwapMemory = meminfo.totalswap;
#else
    mem_info.TotalPhysicalMemory = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

ProcessMemoryUsage GetProcessMemoryUsage() {
    ProcessMemoryUsage usage{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        usage.commit = counters.PrivateUsage;
        usage.working_set = counters.WorkingSetSize;
        usage.private_working_set = counters.PrivateWorkingSetSize;
    }
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        usage.system_commit = u64{performance.CommitTotal} * performance.PageSize;
        usage.system_commit_limit = u64{performance.CommitLimit} * performance.PageSize;
    }
#elif defined(__linux__)
    std::ifstream status{"/proc/self/status"};
    for (std::string line; std::getline(status, line);) {
        const auto field = [&line](std::string_view key) -> u64 {
            if (!line.starts_with(key)) {
                return 0;
            }
            return std::strtoull(line.c_str() + key.size(), nullptr, 10) << 10;
        };
        usage.commit = (std::max)(usage.commit, field("VmData:"));
        usage.working_set = (std::max)(usage.working_set, field("VmRSS:"));
        usage.private_working_set = (std::max)(usage.private_working_set, field("RssAnon:"));
    }
#endif
    return usage;
}

std::optional<s64> GetSystemCommitRoom() {
#ifdef _WIN32
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        const u64 limit = performance.CommitLimit;
        const u64 committed = performance.CommitTotal;
        return static_cast<s64>((committed < limit ? limit - committed : 0) * performance.PageSize);
    }
#elif defined(__APPLE__)
    // Device memory is the system's RAM here, and what does not fit is swapped rather than
    // refused: the room is the RAM this process does not hold (its footprint counts the device
    // memory it owns), negative once it holds more than there is.
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        return static_cast<s64>(GetMemInfo().TotalPhysicalMemory) -
               static_cast<s64>(info.phys_footprint);
    }
#endif
    return std::nullopt;
}

} // namespace Common
