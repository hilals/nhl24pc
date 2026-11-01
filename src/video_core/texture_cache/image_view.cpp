// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>

#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "vulkan/vulkan.hpp"

#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

vk::ImageViewType ConvertImageViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        return vk::ImageViewType::e2DArray;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageViewType::e3D;
    default:
        UNREACHABLE();
    }
}

bool IsViewTypeCompatible(AmdGpu::ImageType view_type, AmdGpu::ImageType image_type) {
    switch (view_type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DMsaaArray:
    case AmdGpu::ImageType::Cube:
        return image_type == AmdGpu::ImageType::Color1D ||
               image_type == AmdGpu::ImageType::Color2D || image_type == AmdGpu::ImageType::Color3D;
    case AmdGpu::ImageType::Color3D:
        return image_type == AmdGpu::ImageType::Color3D;
    default:
        UNREACHABLE();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept
    : is_storage{desc.is_written} {
    const auto dfmt = image.GetDataFmt();
    auto nfmt = image.GetNumberFmt();
    if (is_storage && nfmt == AmdGpu::NumberFormat::Srgb) {
        nfmt = AmdGpu::NumberFormat::Unorm;
    }
    format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);
    if (desc.is_depth) {
        format = Vulkan::LiverpoolToVK::PromoteFormatToDepth(format);
    }

    range.base.level = image.base_level;
    range.base.layer = std::min<u32>(image.base_array, image.NumLayers() - 1);
    range.extent.levels = image.NumViewLevels(desc.is_array);
    range.extent.layers = image.NumViewLayers(desc.is_array);
    type = image.GetViewType(desc.is_array);
    min_lod = static_cast<u32>(image.min_lod);
    if (!is_storage) {
        mapping = image.DstSelect();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept {
    range.base.layer = col_buffer.BaseSlice();
    range.extent.layers = col_buffer.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    format =
        Vulkan::LiverpoolToVK::SurfaceFormat(col_buffer.GetDataFmt(), col_buffer.GetNumberFmt());
}

ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                             AmdGpu::DepthControl ctl) {
    format = Vulkan::LiverpoolToVK::DepthFormat(depth_buffer.z_info.format,
                                                depth_buffer.stencil_info.format);
    is_storage = ctl.depth_write_enable;
    range.base.layer = view.slice_start;
    range.extent.layers = view.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
}

ImageView::ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info_,
                     const Image& image)
    : info{info_} {
    vk::ImageViewUsageCreateInfo usage_ci{.usage = image.usage_flags};
    if (!info.is_storage) {
        usage_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
    // nhlport: the image may have fewer mips than the T# asks for (see ImageInfo::UpdateSize).
    const u32 image_levels = std::max<u32>(image.info.resources.levels, 1u);
    const u32 view_base_level = std::min<u32>(info.range.base.level, image_levels - 1);
    const u32 view_levels =
        std::max<u32>(std::min<u32>(info.range.extent.levels, image_levels - view_base_level), 1u);
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    if (info.min_lod != 0 && instance.IsImageViewMinLodSupported()) {
        const float last_level =
            static_cast<float>(view_base_level + view_levels - 1);
        min_lod_ci.minLod = std::min(static_cast<float>(info.min_lod) / 256.f, last_level);
        usage_ci.pNext = &min_lod_ci;
    }
    // When sampling D32/D16 texture from shader, the T# specifies R32/R16 format so adjust it.
    vk::Format format = info.format;
    vk::ImageAspectFlags aspect = image.aspect_mask;
    bool depth_stencil_view = false;
    if (image.aspect_mask & vk::ImageAspectFlagBits::eDepth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eDepth;
        depth_stencil_view = true;
    }
    if (image.aspect_mask & vk::ImageAspectFlagBits::eStencil &&
        Vulkan::LiverpoolToVK::IsFormatStencilCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eStencil;
        depth_stencil_view = true;
    }
    // nhlport (Vulkan validation, VUID-VkImageViewCreateInfo-image-01761): a colour view of a
    // depth image (e.g. A2B10G10R10 over D32 when the game swaps a buffer between the two) is not
    // allowed in Vulkan and undefined on the GPU. Read the depth instead.
    if ((image.aspect_mask & vk::ImageAspectFlagBits::eDepth) && !depth_stencil_view &&
        format != image.info.pixel_format) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_WARNING(Render_Vulkan, "colour view {} of depth image {} {}x{}, using depth view",
                        vk::to_string(format), vk::to_string(image.info.pixel_format),
                        image.info.size.width, image.info.size.height);
        }
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eDepth;
    }

    const auto components = info.mapping == AmdGpu::IdentityMapping
                                ? vk::ComponentMapping{}
                                : Vulkan::LiverpoolToVK::ComponentMapping(info.mapping);

    const vk::Format view_format = instance.GetSupportedFormat(format, image.format_features);
    // nhlport (Vulkan validation, VUID-VkImageViewCreateInfo-usage-08931): a view inherits every
    // usage of its image, but a reinterpreting view format may not support all of them
    // (E5B9G9R9 can't be a colour attachment on NVIDIA). Keep only what the view format supports.
    {
        const auto feats =
            instance.GetPhysicalDevice().getFormatProperties(view_format).optimalTilingFeatures;
        if (!(feats & vk::FormatFeatureFlagBits::eColorAttachment)) {
            usage_ci.usage &= ~vk::ImageUsageFlagBits::eColorAttachment;
        }
        if (!(feats & vk::FormatFeatureFlagBits::eDepthStencilAttachment)) {
            usage_ci.usage &= ~vk::ImageUsageFlagBits::eDepthStencilAttachment;
        }
        if (!(feats & vk::FormatFeatureFlagBits::eStorageImage)) {
            usage_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
        }
        if (!(feats & vk::FormatFeatureFlagBits::eSampledImage)) {
            usage_ci.usage &= ~vk::ImageUsageFlagBits::eSampled;
        }
        if (!(usage_ci.usage & (vk::ImageUsageFlagBits::eColorAttachment |
                                vk::ImageUsageFlagBits::eDepthStencilAttachment))) {
            usage_ci.usage &= ~vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
        }
        if (!usage_ci.usage) {
            usage_ci.usage = image.usage_flags & (vk::ImageUsageFlagBits::eTransferSrc |
                                                  vk::ImageUsageFlagBits::eTransferDst);
        }
    }

    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = image.GetImage(),
        .viewType = ConvertImageViewType(info.type),
        .format = view_format,
        .components = components,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = view_base_level,
            .levelCount = view_levels,
            .baseArrayLayer = info.range.base.layer,
            .layerCount = info.range.extent.layers,
        },
    };
    if (!IsViewTypeCompatible(info.type, image.info.type)) {
        LOG_ERROR(Render_Vulkan, "image view type {} is incompatible with image type {}",
                  magic_enum::enum_name(info.type), magic_enum::enum_name(image.info.type));
    }

    auto [view_result, view] = instance.GetDevice().createImageViewUnique(image_view_ci);
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(view_result));
    image_view = std::move(view);

    const auto view_aspect = aspect & vk::ImageAspectFlagBits::eDepth     ? "Depth"
                             : aspect & vk::ImageAspectFlagBits::eStencil ? "Stencil"
                                                                          : "Color";
    Vulkan::SetObjectName(
        instance.GetDevice(), *image_view, "ImageView {}x{}x{} {:#x}:{:#x} {}:{} {}:{} ({})",
        image.info.size.width, image.info.size.height, image.info.size.depth,
        image.info.guest_address, image.info.guest_size, info.range.base.level,
        info.range.base.level + info.range.extent.levels - 1, info.range.base.layer,
        info.range.base.layer + info.range.extent.layers - 1, view_aspect);
}

ImageView::~ImageView() = default;

} // namespace VideoCore
