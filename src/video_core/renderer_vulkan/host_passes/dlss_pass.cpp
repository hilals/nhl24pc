// SPDX-FileCopyrightText: Copyright 2026 shadPS4 / nhlport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/host_passes/dlss_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace Vulkan::HostPasses {

static void DlssBridgeLogCallback(int warning, const char* message) {
    if (warning) {
        LOG_WARNING(Render_Vulkan, "[DLSS Bridge] {}", message);
    } else {
        LOG_INFO(Render_Vulkan, "[DLSS Bridge] {}", message);
    }
}

DlssPass::~DlssPass() {
    Destroy();
}

bool DlssPass::Initialize(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device, VmaAllocator allocator, u32 num_images) {
    m_instance = instance;
    m_physical_device = physical_device;
    m_device = device;
    m_allocator = allocator;
    m_num_images = num_images;
    m_images.resize(num_images);

#ifdef _WIN32
    // Look for bbport_dlss.dll next to the executable
    wchar_t exe_path_buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path_buf, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path(exe_path_buf).parent_path();
    std::filesystem::path bridge_path = exe_dir / L"bbport_dlss.dll";

    LOG_INFO(Render_Vulkan, "Searching for DLSS bridge at: {}", bridge_path.string());
    m_bridge_module = LoadLibraryW(bridge_path.c_str());
    if (!m_bridge_module) {
        // Fallback to searching standard library paths
        m_bridge_module = LoadLibraryW(L"bbport_dlss.dll");
    }

    if (!m_bridge_module) {
        LOG_INFO(Render_Vulkan, "bbport_dlss.dll not found; native DLSS will be disabled.");
        m_available = false;
        return false;
    }

    auto get_api = reinterpret_cast<BbDlssGetApiFn>(GetProcAddress(m_bridge_module, "BbDlssGetApi"));
    if (!get_api) {
        LOG_ERROR(Render_Vulkan, "bbport_dlss.dll missing BbDlssGetApi export.");
        FreeLibrary(m_bridge_module);
        m_bridge_module = nullptr;
        m_available = false;
        return false;
    }

    m_api = get_api();
    if (!m_api || m_api->abi != BBPORT_DLSS_BRIDGE_ABI) {
        LOG_ERROR(Render_Vulkan, "bbport_dlss.dll ABI mismatch (got {}, expected {}).",
                  m_api ? m_api->abi : 0, BBPORT_DLSS_BRIDGE_ABI);
        FreeLibrary(m_bridge_module);
        m_bridge_module = nullptr;
        m_api = nullptr;
        m_available = false;
        return false;
    }

    std::filesystem::path cache_dir = exe_dir / L"user" / L"cache" / L"dlss";
    std::filesystem::create_directories(cache_dir);

    std::wstring w_dll_dir = exe_dir.wstring();
    std::wstring w_cache_dir = cache_dir.wstring();

    if (!m_api->Configure(w_dll_dir.c_str(), w_cache_dir.c_str(), DlssBridgeLogCallback)) {
        LOG_ERROR(Render_Vulkan, "DLSS bridge Configure() failed.");
        m_available = false;
        return false;
    }

    auto get_instance_proc = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
    auto get_device_proc = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

    if (!m_api->Initialize(static_cast<VkInstance>(instance),
                           static_cast<VkPhysicalDevice>(physical_device),
                           static_cast<VkDevice>(device),
                           get_instance_proc,
                           get_device_proc)) {
        LOG_WARNING(Render_Vulkan, "DLSS bridge Initialize() failed (no supported RTX GPU or driver).");
        m_available = false;
        return false;
    }

    m_available = true;
    m_initialized = true;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS bridge initialized successfully.");

    CreateFallbackTextures();
    return true;
#else
    return false;
#endif
}

void DlssPass::Destroy() {
    if (m_api) {
        if (m_feature_created) {
            m_api->ReleaseFeature();
            m_feature_created = false;
        }
        if (m_initialized) {
            m_api->Shutdown();
            m_initialized = false;
        }
        m_api = nullptr;
    }

#ifdef _WIN32
    if (m_bridge_module) {
        FreeLibrary(m_bridge_module);
        m_bridge_module = nullptr;
    }
#endif

    m_dummy_depth_view.reset();
    m_dummy_depth_img.Destroy();
    m_dummy_motion_view.reset();
    m_dummy_motion_img.Destroy();

    for (auto& img : m_images) {
        img.output_image_view.reset();
        img.output_image.Destroy();
        img.dirty = true;
    }
    m_available = false;
}

void DlssPass::ResizeAndInvalidate(u32 width, u32 height) {
    m_cur_output_size = vk::Extent2D{.width = width, .height = height};
    for (auto& img : m_images) {
        img.dirty = true;
    }
}

void DlssPass::CreateOutputImages(Img& img) {
    img.dirty = false;
    img.output_image_view.reset();
    img.output_image.Destroy();
    img.output_image = VideoCore::UniqueImage(m_device, m_allocator);

    vk::ImageCreateInfo image_create_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{
            .width = m_cur_output_size.width,
            .height = m_cur_output_size.height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eColorAttachment,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    img.output_image.Create(image_create_info);
    SetObjectName(m_device, static_cast<vk::Image>(img.output_image), "DLSS Output Image");

    vk::ImageViewCreateInfo image_view_create_info{
        .image = img.output_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    img.output_image_view = Check<"create dlss output image view">(m_device.createImageViewUnique(image_view_create_info));
    SetObjectName(m_device, img.output_image_view.get(), "DLSS Output ImageView");
}

void DlssPass::CreateFallbackTextures() {
    if (m_has_fallbacks) return;

    // 1x1 depth fallback (1.0f depth)
    m_dummy_depth_img = VideoCore::UniqueImage(m_device, m_allocator);
    vk::ImageCreateInfo depth_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eD32Sfloat,
        .extent = { .width = 1, .height = 1, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eDepthStencilAttachment,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    m_dummy_depth_img.Create(depth_info);
    m_dummy_depth_view = Check<"create dlss dummy depth view">(m_device.createImageViewUnique(vk::ImageViewCreateInfo{
        .image = m_dummy_depth_img,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eD32Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eDepth,
            .levelCount = 1,
            .layerCount = 1,
        },
    }));

    // 1x1 motion vector fallback (0.0f motion)
    m_dummy_motion_img = VideoCore::UniqueImage(m_device, m_allocator);
    vk::ImageCreateInfo motion_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = { .width = 1, .height = 1, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    m_dummy_motion_img.Create(motion_info);
    m_dummy_motion_view = Check<"create dlss dummy motion view">(m_device.createImageViewUnique(vk::ImageViewCreateInfo{
        .image = m_dummy_motion_img,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    }));

    m_has_fallbacks = true;
}

vk::ImageView DlssPass::Render(vk::CommandBuffer cmdbuf,
                              vk::Image input_image, vk::ImageView input_view, vk::Format input_format,
                              vk::Extent2D input_size, vk::Extent2D output_size,
                              vk::Image depth_image, vk::ImageView depth_view, vk::Format depth_format,
                              vk::Image motion_image, vk::ImageView motion_view, vk::Format motion_format,
                              float jitter_x, float jitter_y,
                              Settings settings, bool hdr) {
    if (!IsAvailable() || !settings.enable) {
        return input_view;
    }

    const bool output_resized = (output_size != m_cur_output_size);
    const bool input_resized = (input_size != m_cur_input_size);
    const bool quality_changed = (settings.quality != m_cur_quality);
    const bool hdr_changed = (hdr != m_cur_hdr);
    const bool needs_feature =
        (!m_feature_created || input_resized || output_resized || quality_changed || hdr_changed);

    if (output_resized) {
        ResizeAndInvalidate(output_size.width, output_size.height);
    }

    auto& img = m_images[m_cur_image];
    if (++m_cur_image >= m_images.size()) {
        m_cur_image = 0;
    }

    if (img.dirty) {
        CreateOutputImages(img);
    }

    // Check if DLSS feature needs to be created or re-created
    if (needs_feature) {
        if (m_feature_created) {
            m_api->ReleaseFeature();
            m_feature_created = false;
        }

        m_cur_input_size = input_size;
        m_cur_output_size = output_size;
        m_cur_quality = settings.quality;
        m_cur_hdr = hdr;

        BbDlssFeature feature_desc{
            .input_width = input_size.width,
            .input_height = input_size.height,
            .output_width = output_size.width,
            .output_height = output_size.height,
            .quality = settings.quality,
            .depth_inverted = 0,
            .preset = 0,
            .hdr = hdr ? 1 : 0,
        };

        if (!m_api->CreateFeature(static_cast<VkCommandBuffer>(cmdbuf), &feature_desc)) {
            LOG_ERROR(Render_Vulkan, "DLSS CreateFeature failed; falling back to input image.");
            return input_view;
        }
        m_feature_created = true;
        LOG_INFO(Render_Vulkan, "DLSS feature created: {}x{} -> {}x{}, quality={}",
                 input_size.width, input_size.height, output_size.width, output_size.height, settings.quality);
    }

    constexpr vk::ImageSubresourceRange color_subresource{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    constexpr vk::ImageSubresourceRange depth_subresource{
        .aspectMask = vk::ImageAspectFlagBits::eDepth,
        .levelCount = 1,
        .layerCount = 1,
    };

    // Transition output image to General layout for DLSS compute write
    const vk::ImageMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = img.output_image,
        .subresourceRange = color_subresource,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    BbDlssEvaluate eval{};
    eval.color = {
        .image = static_cast<VkImage>(input_image),
        .view = static_cast<VkImageView>(input_view),
        .range = static_cast<VkImageSubresourceRange>(color_subresource),
        .format = static_cast<VkFormat>(input_format),
        .width = input_size.width,
        .height = input_size.height,
    };

    if (depth_image && depth_view) {
        eval.depth = {
            .image = static_cast<VkImage>(depth_image),
            .view = static_cast<VkImageView>(depth_view),
            .range = static_cast<VkImageSubresourceRange>(depth_subresource),
            .format = static_cast<VkFormat>(depth_format),
            .width = input_size.width,
            .height = input_size.height,
        };
    } else {
        eval.depth = {
            .image = static_cast<VkImage>(static_cast<vk::Image>(m_dummy_depth_img)),
            .view = static_cast<VkImageView>(m_dummy_depth_view.get()),
            .range = static_cast<VkImageSubresourceRange>(depth_subresource),
            .format = VK_FORMAT_D32_SFLOAT,
            .width = 1,
            .height = 1,
        };
    }

    if (motion_image && motion_view) {
        eval.motion = {
            .image = static_cast<VkImage>(motion_image),
            .view = static_cast<VkImageView>(motion_view),
            .range = static_cast<VkImageSubresourceRange>(color_subresource),
            .format = static_cast<VkFormat>(motion_format),
            .width = input_size.width,
            .height = input_size.height,
        };
    } else {
        eval.motion = {
            .image = static_cast<VkImage>(static_cast<vk::Image>(m_dummy_motion_img)),
            .view = static_cast<VkImageView>(m_dummy_motion_view.get()),
            .range = static_cast<VkImageSubresourceRange>(color_subresource),
            .format = VK_FORMAT_R16G16_SFLOAT,
            .width = 1,
            .height = 1,
        };
    }

    eval.output = {
        .image = static_cast<VkImage>(static_cast<vk::Image>(img.output_image)),
        .view = static_cast<VkImageView>(img.output_image_view.get()),
        .range = static_cast<VkImageSubresourceRange>(color_subresource),
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .width = output_size.width,
        .height = output_size.height,
    };

    eval.jitter_x = jitter_x;
    eval.jitter_y = jitter_y;
    eval.reset = 0;
    eval.frame_ms = 16.666f;
    eval.sharpness = settings.sharpness;

    static uint32_t frame_count = 0;
    if (++frame_count % 300 == 1) {
        LOG_INFO(Render_Vulkan, "DLSS rendering frame {}: {}x{} -> {}x{}, sharpness={:.2f}",
                 frame_count, input_size.width, input_size.height, output_size.width, output_size.height, settings.sharpness);
    }

    if (!m_api->Evaluate(static_cast<VkCommandBuffer>(cmdbuf), &eval)) {
        LOG_WARNING(Render_Vulkan, "DLSS Evaluate failed; falling back to input view.");
        return input_view;
    }

    // Transition output image to ShaderReadOnlyOptimal for presenter
    const vk::ImageMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = img.output_image,
        .subresourceRange = color_subresource,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    return img.output_image_view.get();
}

} // namespace Vulkan::HostPasses
