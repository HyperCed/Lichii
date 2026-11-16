// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

// A device on any Vulkan driver (lavapipe in CI); the test skips without one.
struct TestDevice {
    TestDevice() {
        library = Vulkan::OpenLibrary();
        if (!library) {
            SKIP("No Vulkan library");
        }
        try {
            instance.emplace(Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1));
        } catch (const Vulkan::vk::Exception&) {
            SKIP("No Vulkan instance");
        }
        for (const VkPhysicalDevice physical : instance->EnumeratePhysicalDevices()) {
            try {
                device.emplace(**instance, Vulkan::vk::PhysicalDevice(physical, dld),
                               VK_NULL_HANDLE, dld);
                break;
            } catch (const Vulkan::vk::Exception&) {
                continue;
            }
        }
        if (!device) {
            SKIP("No Vulkan device");
        }
        UNSCOPED_INFO(device->GetDriverName() << " " << device->GetModelName());
    }

    std::shared_ptr<Common::DynamicLibrary> library;
    Vulkan::vk::InstanceDispatch dld;
    std::optional<Vulkan::vk::Instance> instance;
    std::optional<Vulkan::Device> device;
};

VkBufferCreateInfo TooLargeBuffer(const Vulkan::Device& device, VkBufferUsageFlags usage) {
    const VkPhysicalDeviceMemoryProperties memory =
        device.GetPhysical().GetMemoryProperties().memoryProperties;
    VkDeviceSize largest_heap = 0;
    for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
        largest_heap = (std::max)(largest_heap, memory.memoryHeaps[i].size);
    }
    return {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = largest_heap * 2,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
}

} // Anonymous namespace

// #262: a staging buffer the device had no memory for threw from the GPU thread and aborted.
TEST_CASE("Vulkan buffers: TryCreateBuffer returns a null buffer when no heap can hold it",
          "[vulkan_buffer_oom]") {
    TestDevice test;
    const VkBufferCreateInfo too_large =
        TooLargeBuffer(*test.device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Vulkan::MemoryAllocator allocator(*test.device);

    REQUIRE_FALSE(allocator.TryCreateBuffer(too_large, Vulkan::MemoryUsage::Upload));
    REQUIRE_THROWS_AS(allocator.CreateBuffer(too_large, Vulkan::MemoryUsage::Upload),
                      Vulkan::vk::Exception);

    VkBufferCreateInfo fits = too_large;
    fits.size = 64 * 1024;
    const Vulkan::vk::Buffer buffer = allocator.TryCreateBuffer(fits, Vulkan::MemoryUsage::Upload);
    REQUIRE(buffer);
    REQUIRE(buffer.IsHostVisible());
}

// The aligned overload (sparse-compatible buffer cache buffers) shares the plain one's
// allocation path and its retry outside the memory budget, and keeps its alignment.
TEST_CASE("Vulkan buffers: aligned CreateBuffer keeps its alignment on the shared path",
          "[vulkan_buffer_oom]") {
    TestDevice test;
    constexpr VkDeviceSize alignment = 64 * 1024;
    const VkBufferCreateInfo too_large = TooLargeBuffer(
        *test.device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Vulkan::MemoryAllocator allocator(*test.device);

    REQUIRE_THROWS_AS(
        allocator.CreateBuffer(too_large, Vulkan::MemoryUsage::DeviceLocal, alignment),
        Vulkan::vk::Exception);

    VkBufferCreateInfo fits = too_large;
    fits.size = 4096;
    std::vector<Vulkan::vk::Buffer> buffers;
    for (int i = 0; i < 4; ++i) {
        buffers.push_back(allocator.CreateBuffer(fits, Vulkan::MemoryUsage::DeviceLocal, alignment));
        REQUIRE(buffers.back());
        CHECK(buffers.back().Location().offset % alignment == 0);
    }
}
