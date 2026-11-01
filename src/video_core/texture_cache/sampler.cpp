// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

// nhlport: NHLPORT_FORCE_ANISO=<n> raises anisotropic filtering of trilinear colour samplers to at
// least n (2..16). Set to 0 to disable. Defaults to 16.0f.
static float ForcedAnisotropy() {
    static const float value = [] {
        const char* env = std::getenv("NHLPORT_FORCE_ANISO");
        const float v = env ? static_cast<float>(std::atof(env)) : 16.0f;
        if (v > 1.0f) {
            LOG_INFO(Render_Vulkan, "Sampler: forced anisotropic filtering = {:.1f}x", v);
        } else {
            LOG_INFO(Render_Vulkan, "Sampler: forced anisotropic filtering disabled ({:.1f}x)", v);
        }
        return v;
    }();
    return value;
}

// nhlport: NHLPORT_LOD_BIAS=<f> adds f to the mip LOD bias of the same samplers (negative = sharper).
// Defaults to 0.0f (disabled / vanilla PS4 texture sampling). Can be set via NHLPORT_LOD_BIAS if desired.
static float ExtraLodBias() {
    static const float value = [] {
        const char* env = std::getenv("NHLPORT_LOD_BIAS");
        const float v = env ? static_cast<float>(std::atof(env)) : 0.0f;
        if (v != 0.0f) {
            LOG_INFO(Render_Vulkan, "Sampler: extra mip LOD bias = {:.2f}", v);
        } else {
            LOG_INFO(Render_Vulkan, "Sampler: extra mip LOD bias disabled (0.0)");
        }
        return v;
    }();
    return value;
}

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth) {
    using namespace Vulkan;
    const bool game_aniso = AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                            AmdGpu::IsAnisoFilter(sampler.xy_min_filter);
    const float forced_aniso = ForcedAnisotropy();
    const bool trilinear_colour = !is_depth && sampler.mip_filter == AmdGpu::MipFilter::Linear &&
                                  sampler.xy_min_filter != AmdGpu::Filter::Point;
    const bool force_aniso = forced_aniso > 1.0f && trilinear_colour;
    const float max_bias = instance.MaxSamplerLodBias();
    const float lod_bias = std::clamp(
        sampler.LodBias() + (trilinear_colour ? ExtraLodBias() : 0.0f), -max_bias, max_bias);
    const bool anisotropy_enable =
        instance.IsAnisotropicFilteringSupported() && (game_aniso || force_aniso);
    const float wanted_aniso =
        force_aniso ? std::max(sampler.MaxAniso(), forced_aniso) : sampler.MaxAniso();
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(wanted_aniso, 1.0f, instance.MaxSamplerAnisotropy())
                          : 1.0f;
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = lod_bias,
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
