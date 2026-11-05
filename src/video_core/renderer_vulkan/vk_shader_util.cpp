// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/wave64_scratch.h"

namespace Vulkan {

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    // nhlport (from the Be a Pro fork, R11): repair two shared-memory races Vulkan GPU-assisted
    // validation found in the wave64 lowering (wave64_scratch.h). Applied when the module is
    // created, so it also covers shaders loaded from the cache; unrecognised code is unchanged.
    size_t repaired_size{};
    uint32_t repaired_stores{};
    uint32_t* repaired = nhl_wave64_scratch_atomic(code.data(), code.size(), &repaired_size,
                                                   &repaired_stores);
    if (repaired) {
        code = {repaired, repaired_size};
    }
    size_t ordered_size{};
    uint32_t ordered_barriers{};
    uint32_t* ordered = nhl_shared_read_write_barriers(code.data(), code.size(), &ordered_size,
                                                       &ordered_barriers);
    if (ordered) {
        code = {ordered, ordered_size};
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    free(repaired);
    free(ordered);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan
