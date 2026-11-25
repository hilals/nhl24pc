// SPDX-FileCopyrightText: Copyright 2026 shadPS4 / nhlport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"
#include "bbport_dlss_bridge.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan::HostPasses {

class DlssPass {
public:
    struct Settings {
        bool enable{false};
        int quality{1}; // 0 = DLAA, 1 = Quality, 2 = Balanced, 3 = Performance, 4 = Ultra Performance
        float sharpness{0.3f};
    };

    DlssPass() = default;
    ~DlssPass();

    bool Initialize(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device, VmaAllocator allocator, u32 num_images);
    void Destroy();

    bool IsAvailable() const { return m_available && m_initialized; }

    vk::ImageView Render(vk::CommandBuffer cmdbuf,
                         vk::Image input_image, vk::ImageView input_view, vk::Format input_format,
                         vk::Extent2D input_size, vk::Extent2D output_size,
                         vk::Image depth_image, vk::ImageView depth_view, vk::Format depth_format,
                         vk::Image motion_image, vk::ImageView motion_view, vk::Format motion_format,
                         float jitter_x, float jitter_y,
                         Settings settings, bool hdr);

private:
    struct Img {
        VideoCore::UniqueImage output_image;
        vk::UniqueImageView output_image_view;
        bool dirty{true};
    };

    void ResizeAndInvalidate(u32 width, u32 height);
    void CreateOutputImages(Img& img);
    void CreateFallbackTextures();

    vk::Device m_device{};
    vk::PhysicalDevice m_physical_device{};
    vk::Instance m_instance{};
    VmaAllocator m_allocator{};
    u32 m_num_images{0};

    bool m_available{false};
    bool m_initialized{false};
    bool m_feature_created{false};

    HMODULE m_bridge_module{nullptr};
    const BbDlssApi* m_api{nullptr};

    vk::Extent2D m_cur_input_size{};
    vk::Extent2D m_cur_output_size{};
    int m_cur_quality{-1};
    bool m_cur_hdr{false};

    u32 m_cur_image{0};
    std::vector<Img> m_images;

    // Fallback neutral textures
    VideoCore::UniqueImage m_dummy_depth_img;
    vk::UniqueImageView m_dummy_depth_view;
    VideoCore::UniqueImage m_dummy_motion_img;
    vk::UniqueImageView m_dummy_motion_view;
    bool m_has_fallbacks{false};
};

} // namespace Vulkan::HostPasses
