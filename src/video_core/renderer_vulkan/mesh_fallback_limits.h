// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "shader_recompiler/frontend/ir/program.h"

namespace Vulkan {

struct WorkgroupMemoryLayout {
    std::uint64_t size{};

    constexpr void Add(std::uint64_t bytes, std::uint64_t alignment) noexcept {
        size = ((size + alignment - 1) / alignment) * alignment + bytes;
    }
};

[[nodiscard]] inline std::uint64_t MeshFallbackWorkgroupMemorySize(
    const Shader::IR::Program& program) {
    using Shader::Stage;

    if (program.stage != Stage::Task && program.stage != Stage::Mesh) {
        return 0;
    }

    WorkgroupMemoryLayout layout;
    layout.Add(static_cast<std::uint64_t>((std::max)(program.mesh.payload_words, 1U)) *
                   sizeof(std::uint32_t),
               sizeof(std::uint32_t));
    if (program.stage == Stage::Mesh) {
        const Shader::MeshInfo& mesh{program.mesh};
        const auto div_ceil = [](std::uint64_t value, std::uint64_t divisor) {
            return (value + divisor - 1) / divisor;
        };
        const std::uint64_t vertex_lines{mesh.vertex_lines.size()};
        const std::uint64_t primitive_lines{mesh.primitive_lines.size()};
        const std::uint64_t attribute_words{
            (std::max)(div_ceil(mesh.max_vertices, 32) * vertex_lines * 32 +
                           div_ceil(mesh.max_primitives, 32) * primitive_lines * 32,
                       std::uint64_t{1})};
        const std::uint64_t map_elements{
            4 + static_cast<std::uint64_t>(mesh.PrimitiveVertices()) * mesh.max_primitives};
        layout.Add(attribute_words * sizeof(std::uint32_t), sizeof(std::uint32_t));
        layout.Add(map_elements * sizeof(std::uint32_t), sizeof(std::uint32_t));
        layout.Add(sizeof(std::uint32_t), sizeof(std::uint32_t));
    }

    if (program.shared_memory_size != 0) {
        const std::uint64_t shared_size{
            (static_cast<std::uint64_t>(program.shared_memory_size) + 3) & ~std::uint64_t{3}};
        layout.Add(shared_size, sizeof(std::uint32_t));
    }
    return layout.size;
}

// The mesh fallback adds storage-buffer bindings for its output (mesh-only), or
// output, task payload, offsets and indirect arguments (task + mesh). Check
// against the final descriptor set limit without overflowing guest + auxiliary.
[[nodiscard]] constexpr bool FitsStorageBufferSetLimit(std::size_t guest_buffers,
                                                       std::size_t auxiliary_buffers,
                                                       std::size_t set_limit) noexcept {
    return set_limit == 0 ||
           (guest_buffers <= set_limit && auxiliary_buffers <= set_limit - guest_buffers);
}

} // namespace Vulkan
