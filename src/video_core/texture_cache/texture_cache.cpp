// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <array>
#include <span>
#include "common/diag_log.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include <tuple>
#include <limits>
#include <xxhash.h>

#include "common/assert.h"
#include "common/thread.h"
#include "video_core/perf_counters.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include <vulkan/vulkan_format_traits.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u32 MAX_IMAGES = std::numeric_limits<u16>::max();
static constexpr u32 MAX_IMAGE_VIEWS = std::numeric_limits<u16>::max();
static constexpr u32 MAX_SAMPLERS = std::numeric_limits<u16>::max();

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, slot_images{MAX_IMAGES},
      slot_image_views{MAX_IMAGE_VIEWS}, slot_samplers{MAX_SAMPLERS},
      blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    const s64 min_pressure_floor =
        std::clamp<s64>(device_local_memory / 4, 256_MB, DEFAULT_PRESSURE_GC_MEMORY);
    const s64 min_critical_floor =
        std::clamp<s64>(device_local_memory / 2, 512_MB, DEFAULT_CRITICAL_GC_MEMORY);
    pressure_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      min_pressure_floor));
    critical_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      min_critical_floor));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

void TextureCache::ProcessDownloadImages() {
    std::unique_lock lk{download_images_mutex};
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download.mapped, download_size);
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

namespace {
// Debug aid: NHLPORT_DUMP_TEXTURES=<dir> writes each unique uploaded texture (mip 0) as a .dds
// file so UI textures such as player portraits can be found. NHLPORT_DUMP_MIN / NHLPORT_DUMP_MAX
// limit the width/height range (defaults 64..1024).
struct DumpConfig {
    bool enabled{};
    std::filesystem::path dir;
    u32 min_dim{32};
    u32 max_dim{2048};
};

const DumpConfig& GetDumpConfig() {
    static const DumpConfig cfg = [] {
        DumpConfig c;
        if (const char* d = std::getenv("NHLPORT_DUMP_TEXTURES"); d && *d) {
            c.enabled = true;
            c.dir = d;
            std::error_code ec;
            std::filesystem::create_directories(c.dir, ec);
        } else {
            auto auto_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "texture_dump";
            std::error_code ec;
            if (std::filesystem::is_directory(auto_dir, ec)) {
                c.enabled = true;
                c.dir = auto_dir;
            }
        }
        if (const char* v = std::getenv("NHLPORT_DUMP_MIN"); v && *v) {
            c.min_dim = static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        if (const char* v = std::getenv("NHLPORT_DUMP_MAX"); v && *v) {
            c.max_dim = static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        return c;
    }();
    return cfg;
}

struct TextureOverrideEntry {
    u32 width{};
    u32 height{};
    u32 mips{};
    u32 dxgi{};
    std::vector<u8> data;
};

struct TextureOverrideCfg {
    bool enabled{false};
    std::filesystem::path dir;
    std::unordered_map<u64, TextureOverrideEntry> overrides;
    std::unordered_set<u32> target_dims;
};

const TextureOverrideCfg& GetTextureOverrideCfg() {
    static const TextureOverrideCfg cfg = [] {
        TextureOverrideCfg c;
        c.dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "texture_override";
        std::error_code ec;
        if (!std::filesystem::is_directory(c.dir, ec)) {
            return c;
        }
        c.enabled = true;
        for (const auto& entry : std::filesystem::directory_iterator(c.dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".dds") {
                continue;
            }
            const std::string stem = entry.path().stem().string();
            u64 hash = 0;
            size_t last_underscore = stem.rfind('_');
            std::string hash_str = (last_underscore != std::string::npos) ? stem.substr(last_underscore + 1) : stem;
            if (hash_str.size() == 16 && hash_str.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                hash = std::strtoull(hash_str.c_str(), nullptr, 16);
            }
            if (hash == 0) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) {
                continue;
            }
            std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0) {
                continue;
            }
            u32 h = 0, w = 0, mips = 0;
            std::memcpy(&h, file.data() + 12, 4);
            std::memcpy(&w, file.data() + 16, 4);
            std::memcpy(&mips, file.data() + 28, 4);
            if (w == 0 || h == 0) {
                continue;
            }
            if (mips == 0) mips = 1;

            TextureOverrideEntry oe;
            oe.width = w;
            oe.height = h;
            oe.mips = mips;

            size_t data_offset = 128;
            if (std::memcmp(file.data() + 84, "DX10", 4) == 0 && file.size() >= 148) {
                data_offset = 148;
                std::memcpy(&oe.dxgi, file.data() + 128, 4);
            } else if (std::memcmp(file.data() + 84, "DXT1", 4) == 0) {
                oe.dxgi = 71;
            } else if (std::memcmp(file.data() + 84, "DXT5", 4) == 0) {
                oe.dxgi = 77;
            } else if (std::memcmp(file.data() + 84, "BC7", 3) == 0 || std::memcmp(file.data() + 84, "BC7U", 4) == 0) {
                oe.dxgi = 98;
            }

            if (file.size() <= data_offset) {
                continue;
            }
            oe.data.assign(file.begin() + data_offset, file.end());
            c.target_dims.insert((w << 16) | h);
            c.overrides[hash] = std::move(oe);
            LOG_INFO(Render, "texture override loaded: hash {:016x} ({}x{}, mips {}, dxgi {}) from {}",
                     hash, w, h, mips, oe.dxgi, entry.path().filename().string());
        }
        return c;
    }();
    return cfg;
}

u32 DxgiFormatFor(vk::Format f) {
    switch (f) {
    case vk::Format::eR8G8B8A8Unorm: return 28;
    case vk::Format::eR8G8B8A8Srgb: return 29;
    case vk::Format::eB8G8R8A8Unorm: return 87;
    case vk::Format::eB8G8R8A8Srgb: return 91;
    case vk::Format::eR8Unorm: return 61;
    case vk::Format::eBc1RgbaUnormBlock: return 71;
    case vk::Format::eBc1RgbUnormBlock: return 71;
    case vk::Format::eBc1RgbaSrgbBlock: return 72;
    case vk::Format::eBc1RgbSrgbBlock: return 72;
    case vk::Format::eBc2UnormBlock: return 74;
    case vk::Format::eBc2SrgbBlock: return 75;
    case vk::Format::eBc3UnormBlock: return 77;
    case vk::Format::eBc3SrgbBlock: return 78;
    case vk::Format::eBc4UnormBlock: return 80;
    case vk::Format::eBc5UnormBlock: return 83;
    case vk::Format::eBc7UnormBlock: return 98;
    case vk::Format::eBc7SrgbBlock: return 99;
    default: return 0;
    }
}

void WriteDds(const std::filesystem::path& path, u32 w, u32 h, u32 dxgi, bool block, u32 pitch_or_size,
              const u8* data, size_t size) {
    u32 hdr[32] = {};
    hdr[0] = 0x20534444; // "DDS "
    hdr[1] = 124;
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (block ? 0x80000 : 0x8); // CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE/PITCH
    hdr[3] = h;
    hdr[4] = w;
    hdr[5] = pitch_or_size;
    hdr[7] = 1; // mip count
    hdr[19] = 32; // pixel format size
    hdr[20] = 0x4; // DDPF_FOURCC
    hdr[21] = 0x30315844; // "DX10"
    hdr[27] = 0x1000; // DDSCAPS_TEXTURE
    u32 dx10[5] = {dxgi, 3 /*TEXTURE2D*/, 0, 1, 0};
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(hdr), 128);
    out.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

struct PortraitDds {
    bool ok{};
    std::vector<u8> data;
    u32 mips{};
};

PortraitDds LoadPortraitDds(const std::filesystem::path& p) {
    PortraitDds d;
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return d;
    }
    std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0) {
        LOG_WARNING(Render, "portrait override {} is not a DDS file", p.string());
        return d;
    }
    u32 w, h, mips;
    std::memcpy(&h, file.data() + 12, 4);
    std::memcpy(&w, file.data() + 16, 4);
    std::memcpy(&mips, file.data() + 28, 4);
    if (w != 512 || h != 340 || std::memcmp(file.data() + 84, "DXT5", 4) != 0 || mips == 0) {
        LOG_WARNING(Render, "portrait override {} must be 512x340 DXT5 ({}x{}, mips {})", p.string(), w,
                    h, mips);
        return d;
    }
    d.data.assign(file.begin() + 128, file.end());
    d.mips = mips;
    d.ok = true;
    LOG_INFO(Render, "portrait override loaded from {} ({} mips, {} bytes)", p.string(), mips,
             d.data.size());
    return d;
}

struct PortraitCfg {
    std::filesystem::path dir;
    PortraitDds fallback;
    u64 hash{0xc0b20d9c0948575aULL};
    bool dir_exists{};
    std::unordered_set<u32> ids; // players that have a <id>.dds file
};

const PortraitCfg& GetPortraitCfg() {
    static const PortraitCfg cfg = [] {
        PortraitCfg c;
        std::filesystem::path p;
        if (const char* e = std::getenv("NHLPORT_PORTRAIT_OVERRIDE"); e && *e) {
            p = e;
            c.dir = p.parent_path();
        } else {
            c.dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "portrait_override";
            p = c.dir / "silhouette.dds";
        }
        if (const char* h = std::getenv("NHLPORT_SILHOUETTE_HASH"); h && *h) {
            c.hash = std::strtoull(h, nullptr, 16);
        }
        std::error_code ec;
        c.dir_exists = std::filesystem::is_directory(c.dir, ec);
        if (c.dir_exists) {
            c.fallback = LoadPortraitDds(p);
            for (const auto& e : std::filesystem::directory_iterator(c.dir, ec)) {
                const auto name = e.path().stem().string();
                if (e.path().extension() != ".dds" || name.empty() || name.size() > 9 ||
                    name.find_first_not_of("0123456789") != std::string::npos) {
                    continue;
                }
                c.ids.insert(static_cast<u32>(std::strtoul(name.c_str(), nullptr, 10)));
            }
            LOG_INFO(Render, "portrait: {} player pictures found in {}", c.ids.size(),
                     c.dir.string());
        }
        return c;
    }();
    return cfg;
}

// The game keeps small records {player id, 0, 0x13, ...} that change to the id of the player whose
// screen is open. This finds them (ids that have a picture file).
size_t ScanSelectedIdChunk(const u32* p, size_t n, u64* idx_out, size_t cap,
                           const std::unordered_set<u32>& ids, bool any_id = false) {
    size_t found = 0;
#ifdef _MSC_VER
    __try {
#endif
        for (size_t i = 0; i + 3 < n; ++i) {
            if (p[i + 2] == 0x13u && p[i + 1] == 0u && (p[i + 3] == 0x746e6572u || p[i + 3] == 0u) &&
                p[i] != 0u && p[i] < 70000u && (any_id ? p[i] >= 100u : ids.count(p[i]) != 0)) {
                if (found < cap) {
                    idx_out[found] = i;
                }
                ++found;
            }
        }
#ifdef _MSC_VER
    } __except (1) {
        return static_cast<size_t>(-1);
    }
#endif
    return found;
}

std::atomic<u32> selection_generation{0}; // bumped whenever the detected player changes

bool ReadU32Safe(VAddr addr, u32* out) {
#ifdef _MSC_VER
    __try {
        *out = *reinterpret_cast<const volatile u32*>(addr);
    } __except (1) {
        return false;
    }
    return true;
#else
    *out = *reinterpret_cast<const volatile u32*>(addr);
    return true;
#endif
}

// Tracks the "selected player" records. A full scan (slow, ~1-2 s) finds records holding an id that has
// a picture; afterwards only those addresses are read (cheap) and a change in them means another player
// was opened. Returns the id of a player with a picture, or 0 for any other player.
u32 DetectSelectedPlayerId(bool allow_full_scan, bool force_rescan = false) {
    struct Tracked {
        u32 last{};
        u32 changes{};
    };
    static std::mutex mtx;
    static std::unordered_map<VAddr, Tracked> tracked;
    static u32 current = 0;
    static std::chrono::steady_clock::time_point last_full{};
    static bool had_full = false;
    static VAddr win_lo = 0, win_hi = 0; // where the records live (limits later rescans)
    const auto& ids = GetPortraitCfg().ids;
    if (ids.empty()) {
        return 0;
    }
    std::scoped_lock lk{mtx};
    if (tracked.empty() || force_rescan) {
        const auto now = std::chrono::steady_clock::now();
        if (!force_rescan &&
            (!allow_full_scan || (had_full && now - last_full < std::chrono::seconds(15)))) {
            return current;
        }
        const u32 prev_current = current;
        std::unordered_set<VAddr> old_addrs;
        for (const auto& [a, t] : tracked) {
            old_addrs.insert(a);
        }
        tracked.clear();
        had_full = true;
        last_full = now;
        constexpr u64 kChunk = 1u << 20;
        constexpr u64 kOverlap = 16;
        std::vector<u64> idx(65536);
        std::unordered_map<u32, u32> counts;
        std::vector<std::pair<u32, u32>> recs; // (id, word 4 after the id)
        std::vector<VAddr> recs_addr;
        Core::Memory::Instance()->ForEachReadableRange([&](VAddr base, u64 size) {
            if (win_hi != 0) {
                const VAddr lo = std::max(base, win_lo);
                const VAddr hi = std::min<VAddr>(base + size, win_hi);
                if (lo >= hi) {
                    return;
                }
                base = lo;
                size = hi - lo;
            }
            for (u64 off = 0; off < size; off += kChunk - kOverlap) {
                const u64 bytes = std::min<u64>(kChunk, size - off);
                const u32* p = reinterpret_cast<const u32*>(base + off);
                const size_t f = ScanSelectedIdChunk(p, bytes / 4, idx.data(), idx.size(), ids);
                if (f != static_cast<size_t>(-1)) {
                    for (size_t k = 0; k < std::min(f, idx.size()); ++k) {
                        const VAddr a = base + off + idx[k] * 4;
                        if (tracked.emplace(a, Tracked{p[idx[k]], 0}).second) {
                            ++counts[p[idx[k]]];
                            if (idx[k] + 4 < bytes / 4) {
                                recs.emplace_back(p[idx[k]], p[idx[k] + 4]);
                                recs_addr.push_back(a);
                            }
                            if (tracked.size() <= 0 && idx[k] >= 4 && idx[k] + 8 < bytes / 4) {
                                const u32* r = p + idx[k];
                                LOG_INFO(Render,
                                         "portrait: rec {:x} id {} w[-4..7] {:x} {:x} {:x} {:x} | {:x} {:x} {:x} {:x} {:x} {:x} {:x} {:x}",
                                         a, r[0], r[-4], r[-3], r[-2], r[-1], r[0], r[1], r[2], r[3],
                                         r[4], r[5], r[6], r[7]);
                            }
                        }
                    }
                }
                if (off + bytes >= size) {
                    break;
                }
            }
        });
        u32 best = 0, best_n = 0;
        for (const auto& [id, n] : counts) {
            if (n > best_n) {
                best = id;
                best_n = n;
            }
        }
        for (const auto& [id, n] : counts) {
            LOG_INFO(Render, "portrait: full scan id {} x{}", id, n);
        }
        // Most of these records are static roster entries, so the open player can't be told from
        // them yet; it is learned from the records that change when another player is opened.
        if (win_hi == 0 && !tracked.empty()) {
            VAddr lo = ~VAddr{0}, hi = 0;
            for (const auto& [a, t] : tracked) {
                lo = std::min(lo, a);
                hi = std::max(hi, a);
            }
            constexpr VAddr kPad = 128ull << 20;
            win_lo = lo > kPad ? lo - kPad : 0;
            win_hi = hi + kPad;
        }
        // The live record differs from the static roster entries in the word 4 after the id (the
        // static ones all share one value): take the id of the records that stand out.
        current = 0;
        selection_generation.fetch_add(1);
        {
            std::unordered_map<u32, u32> w4;
            u32 mode = 0, mode_n = 0;
            for (const auto& r : recs) {
                if (++w4[r.second] > mode_n) {
                    mode_n = w4[r.second];
                    mode = r.second;
                }
            }
            std::unordered_map<u32, u32> odd;
            // After a rescan only records at new addresses count: odd records that were already
            // there are leftovers of earlier players.
            for (size_t i = 0; i < recs.size(); ++i) {
                if (recs[i].second != mode && (!force_rescan || !old_addrs.count(recs_addr[i]))) {
                    ++odd[recs[i].first];
                }
            }
            u32 best_odd = 0;
            for (const auto& [id, cnt] : odd) {
                if (cnt > best_odd) {
                    best_odd = cnt;
                    current = id;
                }
            }
            if (force_rescan && odd.empty()) {
                current = prev_current; // nothing new found: keep what the watched records said
            }
            LOG_INFO(Render, "portrait: record word4 mode {:x}, {} odd records, initial id {}", mode,
                     odd.size(), current);
        }
        (void)best;
        LOG_INFO(Render, "portrait: full scan tracked {} records, selected id {}", tracked.size(),
                 current);
        return current;
    }
    std::unordered_map<u32, u32> votes;
    for (auto& [addr, t] : tracked) {
        u32 v;
        if (!ReadU32Safe(addr, &v) || v == t.last) {
            continue;
        }
        const bool new_in = ids.count(v) != 0;
        static std::atomic<u32> dbg_n{0};
        if (dbg_n.fetch_add(1) < 300) {
            LOG_INFO(Render, "portrait: record {:x} {} -> {}", addr, t.last, v);
        }
        // Only plausible player ids count; the fields also hold pointers and small counters.
        if (new_in || (v >= 100 && v < 70000)) {
            votes[new_in ? v : 0] += 1 + t.changes;
            ++t.changes;
        }
        t.last = v;
    }
    u32 best = current, best_votes = 0;
    for (const auto& [id, n] : votes) {
        if (n > best_votes) {
            best = id;
            best_votes = n;
        }
    }
    if (best_votes != 0 && best != current) {
        LOG_INFO(Render, "portrait: selected id {} -> {} (votes {})", current, best, best_votes);
        current = best;
        selection_generation.fetch_add(1);
    }
    return current;
}

// nhlport diag (team select): NHLPORT_TS_IDS="13836,20885,26282,..." lists numbers to look for in guest memory when a
// multi-player portrait screen uploads its portraits. Hits that sit close together (the team's featured players) are
// logged with their surroundings, so the portrait order can be tied to player ids. Does nothing when the variable is unset.
size_t ScanForValuesChunk(const u32* p, size_t n, const u64* bits, u64* idx_out, size_t cap) {
    size_t found = 0;
#ifdef _MSC_VER
    __try {
#endif
        for (size_t i = 0; i < n; ++i) {
            const u32 v = p[i];
            if (v < 65536u && ((bits[v >> 6] >> (v & 63)) & 1ull) != 0) {
                if (found < cap) {
                    idx_out[found] = i;
                }
                ++found;
            }
        }
#ifdef _MSC_VER
    } __except (1) {
        return static_cast<size_t>(-1);
    }
#endif
    return found;
}

void DiagScanTeamSelect(u64 frame, VAddr trigger) {
    static const std::vector<u32> wanted = [] {
        std::vector<u32> v;
        if (const char* e = std::getenv("NHLPORT_TS_IDS"); e && *e) {
            const char* s = e;
            while (*s) {
                char* end = nullptr;
                const unsigned long x = std::strtoul(s, &end, 10);
                if (end == s) {
                    ++s;
                    continue;
                }
                if (x < 65536ul) {
                    v.push_back(static_cast<u32>(x));
                }
                s = end;
            }
        }
        return v;
    }();
    if (wanted.empty()) {
        return;
    }
    static std::mutex mtx;
    static u64 last_frame = 0;
    static u32 scans = 0;
    std::scoped_lock lk{mtx};
    if (scans >= 8 || (last_frame != 0 && frame < last_frame + 600)) {
        return;
    }
    last_frame = frame;
    ++scans;
    std::vector<u64> bits(1024, 0);
    for (const u32 v : wanted) {
        bits[v >> 6] |= 1ull << (v & 63);
    }
    LOG_INFO(Render, "portrait-ts: scan {} at frame {} (trigger upload {:x}), {} values", scans, frame, trigger,
             wanted.size());
    std::vector<std::pair<VAddr, u32>> hits;
    constexpr u64 kChunk = 1u << 20;
    constexpr u64 kOverlap = 16;
    std::vector<u64> idx(65536);
    Core::Memory::Instance()->ForEachReadableRange([&](VAddr base, u64 size) {
        for (u64 off = 0; off < size; off += kChunk - kOverlap) {
            const u64 bytes = std::min<u64>(kChunk, size - off);
            const u32* p = reinterpret_cast<const u32*>(base + off);
            const size_t f = ScanForValuesChunk(p, bytes / 4, bits.data(), idx.data(), idx.size());
            if (f != static_cast<size_t>(-1)) {
                for (size_t k = 0; k < std::min(f, idx.size()); ++k) {
                    const VAddr a = base + off + idx[k] * 4;
                    u32 v;
                    if (ReadU32Safe(a, &v) && hits.size() < 400000) {
                        hits.emplace_back(a, v);
                    }
                }
            }
            if (off + bytes >= size) {
                break;
            }
        }
    });
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    std::unordered_map<u32, u32> per_value;
    for (const auto& h : hits) {
        ++per_value[h.second];
    }
    for (const auto& [v, n] : per_value) {
        LOG_INFO(Render, "portrait-ts: value {} found {} times", v, n);
    }
    // clusters: hits closer than 512 bytes that hold at least two different values
    u32 clusters = 0;
    size_t i = 0;
    while (i < hits.size() && clusters < 40) {
        size_t j = i + 1;
        std::unordered_set<u32> vals{hits[i].second};
        while (j < hits.size() && hits[j].first - hits[j - 1].first <= 512) {
            vals.insert(hits[j].second);
            ++j;
        }
        if (vals.size() >= 2) {
            ++clusters;
            const VAddr first = hits[i].first;
            const VAddr last = hits[j - 1].first;
            std::string members;
            for (size_t k = i; k < j && k < i + 24; ++k) {
                members += fmt::format("{}@{:x} ", hits[k].second, hits[k].first);
            }
            LOG_INFO(Render, "portrait-ts: cluster {} {} hits, {} values: {}", clusters, j - i, vals.size(), members);
            const VAddr from = first >= 32 ? first - 32 : first;
            for (VAddr a = from; a <= last + 32 && a < from + 256; a += 32) {
                std::string line;
                for (int w = 0; w < 8; ++w) {
                    u32 v = 0;
                    ReadU32Safe(a + w * 4, &v);
                    line += fmt::format("{:x} ", v);
                }
                LOG_INFO(Render, "portrait-ts:   {:x}: {}", a, line);
            }
        }
        i = j;
    }
    LOG_INFO(Render, "portrait-ts: scan done, {} hits, {} clusters", hits.size(), clusters);
}

struct PortraitState {
    u32 installed{0xFFFFFFFFu};
    u64 last_frame{0};
    u64 settle_until{0};
    u32 settle_gen{0};
    bool need_rescan{false};
    // Uploaded together with other player portraits (team select, lines): the picture there belongs
    // to a roster player, not to the "open player" record, so the game's own silhouette is kept.
    bool multi{false};
};
std::mutex portrait_state_mtx;
std::unordered_map<VAddr, PortraitState> portrait_state;
PortraitDds portrait_original; // the unmodified silhouette, captured on first sight
u64 last_portrait_upload_frame{0};      // guarded by portrait_state_mtx
u64 recent_silhouette_frame{0};         // guarded by portrait_state_mtx
std::vector<VAddr> recent_silhouettes;  // guarded by portrait_state_mtx
constexpr u64 kPortraitBatchFrames = 3;
} // namespace

void TextureCache::InstallPortrait(Image& image, u32 player_id) {
    const auto& cfg = GetPortraitCfg();
    const auto& info = image.info;
    const PortraitDds* src = nullptr;
    if (player_id != 0) {
        static std::mutex cache_mtx;
        static std::unordered_map<u32, PortraitDds> cache;
        std::scoped_lock lk{cache_mtx};
        auto it = cache.find(player_id);
        if (it == cache.end()) {
            it = cache.emplace(player_id, LoadPortraitDds(cfg.dir / fmt::format("{}.dds", player_id))).first;
        }
        if (it->second.ok) {
            src = &it->second;
        }
    }
    if (!src && cfg.fallback.ok) {
        src = &cfg.fallback;
    }
    if (!src && portrait_original.ok) {
        src = &portrait_original;
    }
    {
        std::scoped_lock lk{portrait_state_mtx};
        portrait_state[info.guest_address].installed = player_id;
    }
    LOG_INFO(Render, "portrait: showing picture for player id {} ({})", player_id,
             !src ? "nothing" : src == &portrait_original ? "original" : src == &cfg.fallback ? "fallback" : "player file");
    if (!src) {
        return;
    }
    const u32 levels = std::min<u32>(src->mips, info.resources.levels);
    SmallVector<vk::BufferImageCopy, 12> copies;
    u64 total = 0;
    for (u32 m = 0; m < levels; ++m) {
        const u32 w = std::max(512u >> m, 1u);
        const u32 h = std::max(340u >> m, 1u);
        const u64 size = u64(Common::DivCeil(w, 4u)) * Common::DivCeil(h, 4u) * 16;
        if (total + size > src->data.size()) {
            break;
        }
        copies.push_back({
            .bufferOffset = total,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = m,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {w, h, 1},
        });
        total += size;
    }
    if (copies.empty()) {
        return;
    }
    const auto staging =
        runtime.GetStagingPool().Request(total, MemoryType::HostUncached, 16, false);
    std::memcpy(staging.mapped, src->data.data(), total);
    staging.buffer->Flush(staging.offset, total);
    for (auto& c : copies) {
        c.bufferOffset += staging.offset;
    }
    scheduler.EndRendering();
    runtime.UploadImage(&image, staging.buffer, copies);
}

void TextureCache::RefreshPortraitOnBind(Image& image) {
    const auto& info = image.info;
    if (info.size.width != 512 || info.size.height != 340 ||
        info.pixel_format != vk::Format::eBc3UnormBlock || info.resources.layers != 1 ||
        info.num_samples > 1 || image.usage.render_target || info.props.is_depth) {
        return;
    }
    // The game updates its "open player" record a little after the portrait is first drawn. When the
    // portrait comes back after not being drawn for a while, show the plain silhouette until the
    // record has changed (or a short time has passed) so the previous player's picture is never shown.
    const u64 frame = VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed);
    u32 installed;
    u64 settle_until;
    u32 settle_gen;
    bool need_rescan;
    bool multi;
    {
        std::scoped_lock lk{portrait_state_mtx};
        const auto it = portrait_state.find(info.guest_address);
        if (it == portrait_state.end()) {
            return;
        }
        auto& st = it->second;
        multi = st.multi;
        if (st.installed != 0xFFFFFFFFu && st.last_frame + 20 < frame) {
            st.settle_until = frame + 30;
            st.settle_gen = selection_generation.load();
            st.need_rescan = true;
        }
        st.last_frame = frame;
        installed = st.installed;
        settle_until = st.settle_until;
        settle_gen = st.settle_gen;
        need_rescan = st.need_rescan;
    }
    if (multi) {
        if (installed != 0) {
            InstallPortrait(image, 0);
        }
        return;
    }
    u32 id = DetectSelectedPlayerId(false);
    const bool unchanged = selection_generation.load() == settle_gen;
    if (frame < settle_until && unchanged) {
        id = 0;
    } else if (need_rescan) {
        // Nothing changed in the watched records since the portrait came back: the game may have
        // moved them, so look again (limited to the area where they live).
        if (unchanged) {
            id = DetectSelectedPlayerId(false, true);
        }
        std::scoped_lock lk{portrait_state_mtx};
        portrait_state[info.guest_address].need_rescan = false;
    }
    if (id != installed) {
        InstallPortrait(image, id);
    }
}

void TextureCache::ApplyPortraitOverride(Image& image) {
    // Replaces the grey "no photo" silhouette (512x340 BC3 player portrait). Files are legacy DXT5 .dds
    // with a mip chain, in <user dir>\portrait_override\ (or next to NHLPORT_PORTRAIT_OVERRIDE):
    //   <player id>.dds  picture for that player (id = wBIz in the roster file)
    //   silhouette.dds   optional fallback for every other player without a photo
    // NHLPORT_SILHOUETTE_HASH overrides the content hash of the silhouette.
    const auto& info = image.info;
    if (info.size.width != 512 || info.size.height != 340 ||
        info.pixel_format != vk::Format::eBc3UnormBlock || info.resources.layers != 1 ||
        info.num_samples > 1 || image.usage.render_target || info.props.is_depth) {
        return;
    }
    const auto& cfg = GetPortraitCfg();
    if (!cfg.dir_exists) {
        return;
    }

    // Read mip 0 back and compare its content with the known silhouette.
    const u32 mip0_size = 128 * 85 * 16;
    const auto download =
        runtime.GetStagingPool().Request(mip0_size, MemoryType::HostCached, 16, false);
    const vk::BufferImageCopy dl = {
        .bufferOffset = download.offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {512, 340, 1},
    };
    scheduler.EndRendering();
    runtime.DownloadImage(&image, download.buffer, std::span{&dl, 1});
    scheduler.Finish();
    download.Invalidate();
    const u64 hash = XXH3_64bits(download.mapped, mip0_size);
    static std::atomic<u32> logged{0};
    const bool match = hash == cfg.hash;
    if (logged.fetch_add(1) < 64) {
        LOG_INFO(Render, "portrait upload hash {:016x} {}", hash, match ? "-> silhouette" : "(kept)");
    }
    // Several portraits uploaded within a few frames = a multi-player screen (team select, lines).
    const u64 upload_frame = VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed);
    bool multi = false;
    {
        std::scoped_lock lk{portrait_state_mtx};
        multi = last_portrait_upload_frame != 0 &&
                upload_frame <= last_portrait_upload_frame + kPortraitBatchFrames;
        last_portrait_upload_frame = upload_frame;
        if (upload_frame > recent_silhouette_frame + kPortraitBatchFrames) {
            recent_silhouettes.clear();
        }
        if (multi) {
            for (const VAddr a : recent_silhouettes) {
                const auto it = portrait_state.find(a);
                if (it != portrait_state.end() && !it->second.multi) {
                    it->second.multi = true;
                    LOG_INFO(Render, "portrait: silhouette {:x} is on a multi-player screen", a);
                }
            }
        }
    }
    if (!match) {
        std::scoped_lock lk{portrait_state_mtx};
        portrait_state.erase(info.guest_address);
        return;
    }

    // Keep a copy of the untouched silhouette (all mips) so it can be put back for other players.
    if (!portrait_original.ok) {
        const u32 levels = info.resources.levels;
        SmallVector<vk::BufferImageCopy, 12> copies;
        u64 total = 0;
        for (u32 m = 0; m < levels; ++m) {
            const u32 w = std::max(512u >> m, 1u);
            const u32 h = std::max(340u >> m, 1u);
            copies.push_back({
                .bufferOffset = total,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                     .mipLevel = m,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {w, h, 1},
            });
            total += u64(Common::DivCeil(w, 4u)) * Common::DivCeil(h, 4u) * 16;
        }
        const auto all = runtime.GetStagingPool().Request(total, MemoryType::HostCached, 16, false);
        for (auto& c : copies) {
            c.bufferOffset += all.offset;
        }
        runtime.DownloadImage(&image, all.buffer, copies);
        scheduler.Finish();
        all.Invalidate();
        portrait_original.data.assign(static_cast<const u8*>(all.mapped),
                                      static_cast<const u8*>(all.mapped) + total);
        portrait_original.mips = levels;
        portrait_original.ok = true;
    }
    {
        std::scoped_lock lk{portrait_state_mtx};
        auto& st = portrait_state[info.guest_address];
        st.installed = 0xFFFFFFFFu;
        st.multi = multi;
        recent_silhouettes.push_back(info.guest_address);
        recent_silhouette_frame = upload_frame;
    }
    {
        static std::atomic<u32> ts_log{0};
        if (ts_log.fetch_add(1) < 300) {
            LOG_INFO(Render, "portrait-ts: silhouette upload addr {:x} frame {} multi {}", info.guest_address, upload_frame, multi);
        }
    }
    if (multi) {
        DiagScanTeamSelect(upload_frame, info.guest_address);
    }
    InstallPortrait(image, multi ? 0 : DetectSelectedPlayerId(true));
}

void TextureCache::ApplyTextureOverride(Image& image) {
    // nhlport debug: NHLPORT_ARRAY_BLANK="WxH:FormatSubstr:layerMaskHex;..." zeroes the chosen layers
    // (all mips) of matching texture arrays, so we can find which array layer draws the Coyotes crest.
    // NHLPORT_ARRAY_LOG=1 logs every array image seen.
    {
        const auto& ai = image.info;
        static const std::string blank_spec = [] {
            const char* v = std::getenv("NHLPORT_ARRAY_BLANK");
            std::string s(v ? v : "");
            if (s.empty()) {
                // Same spec from <user>/array_blank.txt (first line), so it survives without env vars.
                std::ifstream in(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "array_blank.txt");
                std::getline(in, s);
                while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
                    s.pop_back();
                }
            }
            if (s.empty()) {
                // Default: hide the leftover Coyotes chest crest / shoulder patches on Utah jerseys
                // (hashes below), leaving every other team's crest layers alone.
                s = "512x512:Bc3Srgb:0f:H";
            }
            return s;
        }();
        static const bool arr_log = [] {
            const char* v = std::getenv("NHLPORT_ARRAY_LOG");
            return v && *v && *v != '0';
        }();
        if ((!blank_spec.empty() || arr_log) && ai.resources.layers > 1 && !ai.props.is_volume &&
            !ai.props.is_depth && ai.num_samples <= 1 && !image.usage.render_target) {
            const u32 aw = ai.size.width;
            const u32 ah = ai.size.height;
            const std::string afmt = vk::to_string(ai.pixel_format);
            if (arr_log) {
                LOG_INFO(Render, "ARRAYSEEN {}x{} {} layers {} levels {} @ {:#x}", aw, ah, afmt,
                         u32(ai.resources.layers), u32(ai.resources.levels), ai.guest_address);
            }
            size_t pos = 0;
            while (pos < blank_spec.size()) {
                size_t end = blank_spec.find(';', pos);
                if (end == std::string::npos) end = blank_spec.size();
                const std::string item = blank_spec.substr(pos, end - pos);
                pos = end + 1;
                const size_t c1 = item.find(':');
                const size_t c2 = item.find(':', c1 == std::string::npos ? 0 : c1 + 1);
                if (c1 == std::string::npos || c2 == std::string::npos) continue;
                if (item.substr(0, c1) != fmt::format("{}x{}", aw, ah)) continue;
                if (afmt.find(item.substr(c1 + 1, c2 - c1 - 1)) == std::string::npos) continue;
                const u64 mask = std::strtoull(item.substr(c2 + 1).c_str(), nullptr, 16);
                // Optional 4th field "WxH:Fmt:mask:N": only blank the Nth (0-based) matching array in
                // the order first seen (by guest address), so one player model's crest can be hit alone.
                const size_t c3 = item.find(':', c2 + 1);
                // "...:H" instead of an index: blank only layers whose content hash (mip 2) is listed in
                // <user>/array_blank_hashes.txt (one hex hash per line). Hashes of layers 0-3 are always logged.
                const bool by_hash = c3 != std::string::npos && item.size() > c3 + 1 && item[c3 + 1] == 'H';
                if (c3 != std::string::npos && !by_hash) {
                    static std::mutex ord_mtx;
                    static std::unordered_map<u64, u32> ord_by_addr;
                    const u32 want = u32(std::strtoul(item.substr(c3 + 1).c_str(), nullptr, 10));
                    u32 got;
                    {
                        std::scoped_lock lk{ord_mtx};
                        auto [oit, ins] = ord_by_addr.try_emplace(ai.guest_address, u32(ord_by_addr.size()));
                        got = oit->second;
                    }
                    if (got != want) continue;
                }
                const auto abx = vk::blockExtent(ai.pixel_format)[0];
                const auto aby = vk::blockExtent(ai.pixel_format)[1];
                const u32 abw = Common::DivCeil(aw, u32(abx));
                const u32 abh = Common::DivCeil(ah, u32(aby));
                const u32 absz = vk::blockSize(ai.pixel_format);
                const u64 zsize = u64(abw) * abh * absz;
                const auto zbuf = runtime.GetStagingPool().Request(zsize, MemoryType::HostUncached, 16, false);
                std::memset(zbuf.mapped, 0, zsize);
                zbuf.buffer->Flush(zbuf.offset, zsize);
                std::vector<vk::BufferImageCopy> zc;
                const u32 lv = u32(ai.resources.levels);
                u64 layer_hash[4]{};
                {
                    const u32 hm = std::min<u32>(2, lv - 1);
                    const u32 hw = std::max(aw >> hm, 1u);
                    const u32 hh = std::max(ah >> hm, 1u);
                    const u64 hsz = u64(Common::DivCeil(hw, u32(abx))) * Common::DivCeil(hh, u32(aby)) * absz;
                    const u32 nl = std::min<u32>(ai.resources.layers, 4);
                    const auto hbuf = runtime.GetStagingPool().Request(hsz * nl, MemoryType::HostCached, 16, false);
                    std::vector<vk::BufferImageCopy> hc;
                    for (u32 l = 0; l < nl; ++l) {
                        hc.push_back({
                            .bufferOffset = hbuf.offset + l * hsz,
                            .bufferRowLength = 0,
                            .bufferImageHeight = 0,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = hm,
                                                 .baseArrayLayer = l,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {hw, hh, 1},
                        });
                    }
                    scheduler.EndRendering();
                    runtime.DownloadImage(&image, hbuf.buffer, hc);
                    scheduler.Finish();
                    hbuf.Invalidate();
                    for (u32 l = 0; l < nl; ++l) {
                        layer_hash[l] = XXH3_64bits(static_cast<const u8*>(hbuf.mapped) + l * hsz, hsz);
                    }
                    static std::mutex hl_mtx;
                    static std::unordered_set<u64> hl_seen;
                    std::scoped_lock lk{hl_mtx};
                    if (hl_seen.insert(ai.guest_address ^ layer_hash[0] ^ (layer_hash[1] << 1)).second) {
                        LOG_INFO(Render, "ARRAYHASH @ {:#x} layers {} L0 {:016x} L1 {:016x} L2 {:016x} L3 {:016x}",
                                 ai.guest_address, u32(ai.resources.layers), layer_hash[0], layer_hash[1],
                                 layer_hash[2], layer_hash[3]);
                    }
                }
                static const std::unordered_set<u64> blank_hashes = [] {
                    // Coyotes chest kachina (array L0) and shoulder patches (array L1/L2).
                    std::unordered_set<u64> s{0xba10aa88955cd737ULL, 0xe69f43c5d25dcaebULL};
                    std::ifstream in(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "array_blank_hashes.txt");
                    std::string line;
                    while (std::getline(in, line)) {
                        const u64 h = std::strtoull(line.c_str(), nullptr, 16);
                        if (h != 0) s.insert(h);
                    }
                    return s;
                }();
                for (u32 layer = 0; layer < std::min<u32>(ai.resources.layers, 64); ++layer) {
                    if (!((mask >> layer) & 1)) continue;
                    if (by_hash && (layer >= 4 || blank_hashes.count(layer_hash[layer]) == 0)) continue;
                    for (u32 m = 0; m < lv; ++m) {
                        const u32 mw = std::max(aw >> m, 1u);
                        const u32 mh = std::max(ah >> m, 1u);
                        zc.push_back({
                            .bufferOffset = zbuf.offset,
                            .bufferRowLength = 0,
                            .bufferImageHeight = 0,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = m,
                                                 .baseArrayLayer = layer,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {mw, mh, 1},
                        });
                    }
                }
                if (!zc.empty()) {
                    scheduler.EndRendering();
                    runtime.UploadImage(&image, zbuf.buffer, zc);
                    LOG_INFO(Render, "ARRAYBLANK {}x{} {} mask {:x} @ {:#x} ({} copies)", aw, ah, afmt, mask,
                             ai.guest_address, zc.size());
                }
            }
        }
    }
    const auto& info = image.info;
    if (info.props.is_depth || info.num_samples > 1 || info.resources.layers != 1 ||
        info.props.is_volume || image.usage.render_target) {
        return;
    }
    const auto& cfg = GetTextureOverrideCfg();
    if (!cfg.enabled || cfg.overrides.empty()) {
        return;
    }
    const u32 w = info.size.width;
    const u32 h = info.size.height;
    const bool is_ui_cand = (w <= 512 && h <= 512 && w >= 32 && h >= 32 && (w == h || (w == 256 && h == 512) || (w == 512 && h == 256)));
    static const bool log_all = [] {
        const char* v = std::getenv("NHLPORT_LOG_BIND");
        return v && *v && *v != '0';
    }();
    if (!is_ui_cand && cfg.target_dims.count((w << 16) | h) == 0 && !log_all) {
        return;
    }

    const auto bx = vk::blockExtent(info.pixel_format)[0];
    const auto by = vk::blockExtent(info.pixel_format)[1];
    const u32 blocks_w = Common::DivCeil(w, u32(bx));
    const u32 blocks_h = Common::DivCeil(h, u32(by));
    const u32 bsize = vk::blockSize(info.pixel_format);
    const u32 mip0_size = blocks_w * blocks_h * bsize;

    const auto download = runtime.GetStagingPool().Request(mip0_size, MemoryType::HostCached, 16, false);
    const vk::BufferImageCopy dl = {
        .bufferOffset = download.offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {w, h, 1},
    };
    scheduler.EndRendering();
    runtime.DownloadImage(&image, download.buffer, std::span{&dl, 1});
    scheduler.Finish();
    download.Invalidate();

    const u8* data = reinterpret_cast<const u8*>(download.mapped);
    const u64 hash = XXH3_64bits(data, mip0_size);
    image.content_hash = hash;

    if (is_ui_cand) {
        LOG_INFO(Render, "UI tex {}x{} format {} hash {:016x} (override: {})", w, h, vk::to_string(info.pixel_format), hash, cfg.overrides.count(hash) != 0);
    } else {
        LOG_INFO(Render, "Gear/Jersey tex {}x{} format {} hash {:016x} (override: {})", w, h, vk::to_string(info.pixel_format), hash, cfg.overrides.count(hash) != 0);
    }

    auto it = cfg.overrides.find(hash);
    if (it == cfg.overrides.end()) {
        return;
    }

    const auto& oe = it->second;
    LOG_INFO(Render, "Applying texture override for hash {:016x} ({}x{})", hash, w, h);

    const u32 levels = std::min<u32>(oe.mips, info.resources.levels);
    SmallVector<vk::BufferImageCopy, 12> copies;
    u64 total = 0;
    for (u32 m = 0; m < levels; ++m) {
        const u32 mw = std::max(w >> m, 1u);
        const u32 mh = std::max(h >> m, 1u);
        const u32 mbw = Common::DivCeil(mw, u32(bx));
        const u32 mbh = Common::DivCeil(mh, u32(by));
        const u64 msize = u64(mbw) * mbh * bsize;
        if (total + msize > oe.data.size()) {
            break;
        }
        copies.push_back({
            .bufferOffset = total,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = m,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {mw, mh, 1},
        });
        total += msize;
    }
    if (copies.empty()) {
        return;
    }
    const auto staging =
        runtime.GetStagingPool().Request(total, MemoryType::HostUncached, 16, false);
    std::memcpy(staging.mapped, oe.data.data(), total);
    staging.buffer->Flush(staging.offset, total);
    for (auto& c : copies) {
        c.bufferOffset += staging.offset;
    }
    scheduler.EndRendering();
    runtime.UploadImage(&image, staging.buffer, copies);
    LOG_INFO(Render, "Successfully replaced texture {:016x} ({} mips uploaded)", hash, copies.size());
}

void TextureCache::DumpImageToDisk(Image& image) {
    // NHLPORT_PORTRAIT_CTX=<dir>: for 512x340 BC3 (player portrait) uploads, save the guest memory
    // around the texture so the player id / name that selected it can be searched for offline.
    if (const char* ctx_dir = std::getenv("NHLPORT_PORTRAIT_CTX"); ctx_dir && *ctx_dir) {
        const auto& pi = image.info;
        static std::atomic<u32> ctx_count{0};
        if (pi.size.width == 512 && pi.size.height == 340 &&
            pi.pixel_format == vk::Format::eBc3UnormBlock && ctx_count.load() < 24) {
            constexpr u64 kPad = 0x40000;
            const VAddr lo = pi.guest_address > kPad ? pi.guest_address - kPad : pi.guest_address;
            const VAddr hi = pi.guest_address + pi.guest_size + kPad;
            auto* mem = Core::Memory::Instance();
            if (mem->IsValidMapping(lo, hi - lo)) {
                std::error_code ec;
                std::filesystem::create_directories(ctx_dir, ec);
                const auto name = fmt::format("ctx_{}_a{:x}_off{:x}.bin", ctx_count.fetch_add(1),
                                              pi.guest_address, pi.guest_address - lo);
                std::ofstream out(std::filesystem::path(ctx_dir) / name, std::ios::binary);
                out.write(reinterpret_cast<const char*>(lo), static_cast<std::streamsize>(hi - lo));
            }
        }
    }
    const DumpConfig& cfg = GetDumpConfig();
    if (!cfg.enabled) {
        return;
    }
    const auto& info = image.info;
    if (info.props.is_depth || info.num_samples > 1 || info.resources.layers != 1 ||
        info.props.is_volume || image.usage.render_target) {
        return;
    }
    const u32 w = info.size.width;
    const u32 h = info.size.height;
    if (w < cfg.min_dim || h < cfg.min_dim || w > cfg.max_dim || h > cfg.max_dim) {
        return;
    }
    const u32 dxgi = DxgiFormatFor(info.pixel_format);
    if (dxgi == 0) {
        return;
    }
    const auto bx = vk::blockExtent(info.pixel_format)[0];
    const auto by = vk::blockExtent(info.pixel_format)[1];
    const bool block = bx > 1;
    const u32 blocks_w = Common::DivCeil(w, u32(bx));
    const u32 blocks_h = Common::DivCeil(h, u32(by));
    const u32 bsize = vk::blockSize(info.pixel_format);
    const u32 size = blocks_w * blocks_h * bsize;

    const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16, false);
    const vk::BufferImageCopy copy = {
        .bufferOffset = download.offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {w, h, 1},
    };
    scheduler.EndRendering();
    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
    scheduler.Finish();
    download.Invalidate();

    const u8* data = reinterpret_cast<const u8*>(download.mapped);
    const u64 hash = XXH3_64bits(data, size);
    {
        static std::mutex mtx;
        static std::unordered_set<u64> seen;
        std::scoped_lock lk{mtx};
        if (!seen.insert(hash).second) {
            return;
        }
    }
    const std::string name = fmt::format("{}x{}_{}_{:016x}_a{:x}.dds", w, h, vk::to_string(info.pixel_format),
                                         hash, info.guest_address);
    LOG_INFO(Render, "Dumped texture: {}", name);
    WriteDds(cfg.dir / name, w, h, dxgi, block, block ? blocks_w * blocks_h * bsize : w * (bsize), data, size);
}

// nhlport debug: texture arrays / other images that DumpImageToDisk and ApplyTextureOverride skip.
// Logs why, and for arrays (layers > 1) dumps mip 0 of every layer as its own .dds file.
void TextureCache::DebugDumpLayers(Image& image) {
    const auto& info = image.info;
    if (info.props.is_depth || info.num_samples > 1 || image.usage.render_target) {
        return;
    }
    const DumpConfig& cfg = GetDumpConfig();
    if (!cfg.enabled) {
        return;
    }
    const u32 w = info.size.width;
    const u32 h = info.size.height;
    const u32 layers = info.resources.layers;
    if (info.resources.layers == 1 && !info.props.is_volume) {
        return; // normal textures are handled by the regular paths
    }
    LOG_INFO(Render, "ARRAYTEX {}x{} {} layers {} levels {} volume {} @ {:#x}", w, h,
             vk::to_string(info.pixel_format), layers, u32(info.resources.levels), u32(info.props.is_volume),
             info.guest_address);
    if (info.props.is_volume || w < 32 || h < 32 || w > 2048 || h > 2048) {
        return;
    }
    const u32 dxgi = DxgiFormatFor(info.pixel_format);
    if (dxgi == 0) {
        return;
    }
    const auto bx = vk::blockExtent(info.pixel_format)[0];
    const auto by = vk::blockExtent(info.pixel_format)[1];
    const bool block = bx > 1;
    const u32 blocks_w = Common::DivCeil(w, u32(bx));
    const u32 blocks_h = Common::DivCeil(h, u32(by));
    const u32 bsize = vk::blockSize(info.pixel_format);
    const u32 size = blocks_w * blocks_h * bsize;
    for (u32 layer = 0; layer < std::min<u32>(layers, 64); ++layer) {
        const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16, false);
        const vk::BufferImageCopy copy = {
            .bufferOffset = download.offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = 0,
                                 .baseArrayLayer = layer,
                                 .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {w, h, 1},
        };
        scheduler.EndRendering();
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const u8* data = reinterpret_cast<const u8*>(download.mapped);
        const u64 hash = XXH3_64bits(data, size);
        static std::mutex mtx;
        static std::unordered_set<u64> seen;
        {
            std::scoped_lock lk{mtx};
            if (!seen.insert(hash).second) {
                continue;
            }
        }
        const std::string name = fmt::format("{}x{}_{}_{:016x}_L{}_a{:x}.dds", w, h,
                                             vk::to_string(info.pixel_format), hash, layer,
                                             info.guest_address);
        LOG_INFO(Render, "ARRAYLAYER {} layer {}/{} hash {:016x}", name, layer, layers, hash);
        WriteDds(cfg.dir / name, w, h, dxgi, block, block ? blocks_w * blocks_h * bsize : w * bsize, data,
                 size);
    }
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

static bool StreamedTexturesEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("NHLPORT_STREAMED");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

static bool SkipSmallGpuInvalidations() {
    static const bool enabled = [] {
        const char* env = std::getenv("NHLPORT_GPU_INVAL_SMALL");
        return !(env && env[0] == '1');
    }();
    return enabled;
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size, bool from_cp) {
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);

    SmallVector<ImageId, 8> image_ids;
    ForEachImageInRegion(pages_start, pages_end - pages_start,
                         [&](ImageId image_id, Image&) { image_ids.push_back(image_id); });

    for (const auto image_id : image_ids) {
        Image& image = slot_images[image_id];
        std::scoped_lock lk{image.mutex};

        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            if (VideoCore::Perf::draw_trace.load(std::memory_order_relaxed)) {
                VideoCore::Perf::DrawLog(fmt::format(
                    "cpu-inval f{} d{} addr {:#x} size {} -> id {} {}x{} {} @ {:#x} rt {} gpumod {} thread {}",
                    VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed),
                    VideoCore::Perf::draw_index.load(std::memory_order_relaxed), addr, size, image_id.index,
                    image.info.size.width, image.info.size.height, vk::to_string(image.info.pixel_format),
                    image.info.guest_address, u32(image.usage.render_target),
                    u32(True(image.flags & ImageFlagBits::GpuModified)), Common::GetCurrentThreadName()));
            }
            (from_cp ? Diag::C::tc_cp_inval : Diag::C::tc_cpu_inval)
                .fetch_add(1, std::memory_order_relaxed);
            if (from_cp) {
                // Render targets and GPU-written images keep their GPU copy; the inline write
                // is patched into the buffer cache. Sampled textures (glyph pages) re-upload.
                if (image.usage.render_target || image.usage.depth_target ||
                    True(image.flags & ImageFlagBits::GpuModified)) {
                    continue;
                }
                image.cp_written = true;
                image.streamed = false;
                image.flags |= ImageFlagBits::CpuDirty;
                continue;
            }
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
            ++image.cpu_fault_count;
            {
                const u64 frame = VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed);
                if (image.last_fault_frame != frame) {
                    image.last_fault_frame = frame;
                    ++image.fault_frames;
                }
            }
            if (image.fault_frames >= 8 && !image.streamed && !image.cp_written && StreamedTexturesEnabled() &&
                !image.info.props.is_block && image.info.guest_size <= (8u << 20) &&
                image.info.guest_size >= (256u << 10)) {
                image.streamed = true;
                VideoCore::Perf::streamed_images.fetch_add(1, std::memory_order_relaxed);
                LOG_INFO(Render, "streamed texture: id {} {}x{} {} @ {:#x} (uploaded once per frame)",
                         image_id.index, image.info.size.width, image.info.size.height,
                         vk::to_string(image.info.pixel_format), image.info.guest_address);
            }
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    }
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // nhlport: NHL 24's compute metadata clears (16 KB CMASK-sized writes) land on the base
        // address of live 1080p render / depth targets every frame; reloading the whole 8 MB
        // target from the buffer cache for that is wasted work (the target is cleared or fully
        // redrawn before use anyway). Writes smaller than 1/8 of a GPU-rendered target are ignored
        // (NHLPORT_GPU_INVAL_SMALL=1 restores the upstream behaviour).
        if (SkipSmallGpuInvalidations() && (image.usage.render_target || image.usage.depth_target) &&
            True(image.flags & ImageFlagBits::GpuModified) && max_size * 8 < image.info.guest_size) {
            VideoCore::Perf::gpu_inval_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Ensure image is reuploaded when accessed again.
        if (VideoCore::Perf::draw_trace.load(std::memory_order_relaxed)) {
            VideoCore::Perf::DrawLog(fmt::format(
                "gpu-inval f{} d{} src {} addr {:#x} size {:#x} -> id {} {}x{} {} @ {:#x} rt {} gpumod {}",
                VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed),
                VideoCore::Perf::draw_index.load(std::memory_order_relaxed), VideoCore::Perf::inval_source,
                address, max_size, image_id.index, image.info.size.width, image.info.size.height,
                vk::to_string(image.info.pixel_format), image.info.guest_address,
                u32(image.usage.render_target), u32(True(image.flags & ImageFlagBits::GpuModified))));
        }
        Diag::C::tc_gpu_inval.fetch_add(1, std::memory_order_relaxed);
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    SmallVector<ImageId, 16> deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        VideoCore::Perf::NoteFree(VideoCore::Perf::FreeUnmap);
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        VideoCore::Perf::tc_depth_recreate.fetch_add(1, std::memory_order_relaxed);
        {
            static std::atomic<u64> n{0};
            const u64 k = n.fetch_add(1, std::memory_order_relaxed);
            if (k < 60 || k % 2000 == 0) {
                LOG_INFO(Render, "depth-recreate #{} binding {}: cache {}x{} {} mips {} layers {} depth {} -> req {}x{} {} mips {} layers {} depth {} @ {:#x}",
                         k, static_cast<u32>(binding), cache_image.info.size.width, cache_image.info.size.height,
                         vk::to_string(cache_image.info.pixel_format), cache_image.info.resources.levels,
                         cache_image.info.resources.layers, u32(cache_image.info.props.is_depth),
                         requested_info.size.width, requested_info.size.height,
                         vk::to_string(requested_info.pixel_format), requested_info.resources.levels,
                         requested_info.resources.layers, u32(requested_info.props.is_depth),
                         requested_info.guest_address);
            }
        }
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        new_info.UpdateSize();
        const auto new_image_id = slot_images.Insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        if (new_info.num_samples == 1 &&
            (new_info.resources.layers > cache_image.info.resources.layers ||
             new_info.resources.levels > cache_image.info.resources.levels)) {
            RefreshImage(new_image);
        }
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        // nhlport: NHL 24 aliases its depth buffer memory with a colour render target of the same
        // size and switches between the two every frame. Reinterpreting the old bits is only
        // meaningful when the new image is going to be *read* (texture / storage); for a render or
        // depth target the game overwrites the surface and the copied bits (depth shown as colour)
        // only cost a full-size copy per frame and leak garbage into untouched regions.
        const bool contents_matter =
            binding == BindingType::Texture || binding == BindingType::Storage;
        if (contents_matter) {
            runtime.CopyColorAndDepth(&cache_image, &new_image);
        } else {
            VideoCore::Perf::tc_depth_skip.fetch_add(1, std::memory_order_relaxed);
        }

        // Free the cache image.
        VideoCore::Perf::NoteFree(VideoCore::Perf::FreeDepthRecreate);
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    static constexpr u64 NUM_FRAMES_BEFORE_REMOVAL = 32;
    VideoCore::Perf::tc_overlap.fetch_add(1, std::memory_order_relaxed);
    VideoCore::Perf::current_request = &image_info;
    VideoCore::Perf::current_binding = static_cast<u32>(binding);

    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NUM_FRAMES_BEFORE_REMOVAL;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                VideoCore::Perf::NoteFree(VideoCore::Perf::FreePoolAlias);
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            if (!is_compatible) {
                static std::atomic<u64> n{0};
                const u64 k = n.fetch_add(1, std::memory_order_relaxed);
                if (k < 60 || k % 2000 == 0) {
                    LOG_INFO(Render, "incompatible view #{}: binding {} wants {}x{} {} but cache has {}x{} {} mips {} depth {} @ {:#x}",
                             k, static_cast<u32>(binding), image_info.size.width, image_info.size.height,
                             vk::to_string(image_info.pixel_format), result_image.info.size.width,
                             result_image.info.size.height, vk::to_string(result_image.info.pixel_format),
                             result_image.info.resources.levels, u32(result_image.info.props.is_depth),
                             image_info.guest_address);
                }
                VideoCore::Perf::tc_incompatible.fetch_add(1, std::memory_order_relaxed);
            }
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                VideoCore::Perf::NoteFree(VideoCore::Perf::FreeTileMode);
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Same format and tiling, larger guest size but fewer resources: the address was reused
        // for a different image. Log the first few and recover instead of aborting.
        static std::atomic<u64> unresolved_overlaps{0};
        if (unresolved_overlaps.fetch_add(1, std::memory_order_relaxed) < 3) {
            // Enhanced debug logging for unreachable case
            // Calculate expected size based on format and dimensions
            u64 expected_size =
                (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
                 static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
            LOG_ERROR(Render_Vulkan,
                      "Unresolvable image overlap with equal memory address:\n"
                      "=== OLD IMAGE (cached) ===\n"
                      "  Address:        {:#x}\n"
                      "  Size:           {:#x} bytes\n"
                      "  Format:         {}\n"
                      "  Type:           {}\n"
                      "  Width:          {}\n"
                      "  Height:         {}\n"
                      "  Depth:          {}\n"
                      "  Pitch:          {}\n"
                      "  Mip levels:     {}\n"
                      "  Array layers:   {}\n"
                      "  Samples:        {}\n"
                      "  Tile mode:      {:#x}\n"
                      "  Block size:     {} bits\n"
                      "  Is block-comp:  {}\n"
                      "  Guest size:     {:#x}\n"
                      "  Last accessed:  tick {}\n"
                      "  Safe to delete: {}\n"
                      "  isPow2:         {}\n"
                      "  Alt tile:       {}\n"
                      "\n"
                      "=== NEW IMAGE (requested) ===\n"
                      "  Address:        {:#x}\n"
                      "  Size:           {:#x} bytes\n"
                      "  Format:         {}\n"
                      "  Type:           {}\n"
                      "  Width:          {}\n"
                      "  Height:         {}\n"
                      "  Depth:          {}\n"
                      "  Pitch:          {}\n"
                      "  Mip levels:     {}\n"
                      "  Array layers:   {}\n"
                      "  Samples:        {}\n"
                      "  Tile mode:      {:#x}\n"
                      "  Block size:     {} bits\n"
                      "  Is block-comp:  {}\n"
                      "  Guest size:     {:#x}\n"
                      "  isPow2:         {}\n"
                      "  Alt tile:       {}\n"
                      "\n"
                      "=== COMPARISON ===\n"
                      "  Same format:           {}\n"
                      "  Same type:             {}\n"
                      "  Same tile mode:        {}\n"
                      "  Same block size:       {}\n"
                      "  Same BlockDim:         {}\n"
                      "  Same pitch:            {}\n"
                      "  Same pow2:             {}\n"
                      "  Same alt tile:         {}\n"
                      "  Old resources <= new:  {} (old: {}, new: {})\n"
                      "  Old size <= new size:  {}\n"
                      "  Expected size (calc):  {} bytes\n"
                      "  Size ratio (new/expected): {:.2f}x\n"
                      "  Size ratio (new/old):  {:.2f}x\n"
                      "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                      "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                      "  Merged image ID:       {}\n"
                      "  Binding type:          {}\n"
                      "  Current tick:          {}\n"
                      "  Age (ticks since last access): {}",

                      // Old image details
                      cache_image.info.guest_address, cache_image.info.guest_size,
                      vk::to_string(cache_image.info.pixel_format),
                      static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                      cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                      cache_image.info.resources.levels, cache_image.info.resources.layers,
                      cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                      cache_image.info.num_bits, +cache_image.info.props.is_block,
                      cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                      bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                      // New image details
                      image_info.guest_address, image_info.guest_size,
                      vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                      image_info.size.width, image_info.size.height, image_info.size.depth,
                      image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                      image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                      image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                      bool(image_info.props.is_pow2), image_info.alt_tile,

                      // Comparison
                      (image_info.pixel_format == cache_image.info.pixel_format),
                      (image_info.type == cache_image.info.type),
                      (image_info.tile_mode == cache_image.info.tile_mode),
                      (image_info.num_bits == cache_image.info.num_bits),
                      (image_info.BlockDim() == cache_image.info.BlockDim()),
                      (image_info.pitch == cache_image.info.pitch),
                      (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                      (image_info.alt_tile == cache_image.info.alt_tile),
                      (cache_image.info.resources <= image_info.resources),
                      cache_image.info.resources.levels, image_info.resources.levels,
                      (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                      // Size ratios
                      static_cast<double>(image_info.guest_size) / expected_size,
                      static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                      // Difference between actual and expected sizes with percentages
                      static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                      (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                      static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                      (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                      merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                      scheduler.CurrentTick() - cache_image.tick_accessed_last);
        }

        if (safe_to_delete) {
            VideoCore::Perf::NoteFree(VideoCore::Perf::FreePoolAlias);
            FreeImage(cache_image_id);
        }
        return {merged_image_id, -1, -1};
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                {
                    static std::atomic<u64> n{0};
                    const u64 k = n.fetch_add(1, std::memory_order_relaxed);
                    if (k < 60 || k % 5000 == 0) {
                        LOG_INFO(Render, "right-overlap view #{}: binding {} {}x{} {} @ {:#x} = mip {} slice {} of {}x{} {} mips {} layers {} @ {:#x} (idle {} ticks)",
                                 k, static_cast<u32>(binding), image_info.size.width, image_info.size.height,
                                 vk::to_string(image_info.pixel_format), image_info.guest_address, mip, slice,
                                 cache_image.info.size.width, cache_image.info.size.height,
                                 vk::to_string(cache_image.info.pixel_format), cache_image.info.resources.levels,
                                 cache_image.info.resources.layers, cache_image.info.guest_address,
                                 scheduler.CurrentTick() - cache_image.tick_accessed_last);
                    }
                }
                VideoCore::Perf::tc_right_view.fetch_add(1, std::memory_order_relaxed);
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            VideoCore::Perf::NoteFree(VideoCore::Perf::FreeRightChance);
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        auto& merged_image = GetImage(merged_image_id);
                        merged_image.binding.is_target = 1u;
                        // nhlport: the subresource may already hold rendered data (an earlier
                        // draw into this mip); on real hardware it is the same memory, so carry
                        // it over instead of leaving the merged mip with stale guest bytes.
                        if (True(cache_image.flags & ImageFlagBits::GpuModified)) {
                            VideoCore::Perf::tc_target_copy.fetch_add(1, std::memory_order_relaxed);
                            runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                        }
                    }

                    VideoCore::Perf::NoteFree(VideoCore::Perf::FreeLeftTarget);
                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    {
                        static std::atomic<u64> n{0};
                        const u64 k = n.fetch_add(1, std::memory_order_relaxed);
                        if (k < 60 || k % 2000 == 0) {
                            LOG_INFO(Render, "left-overlap copy #{}: binding {} {}x{} {} @ {:#x} (idle {} ticks) -> mip {} slice {} of {}x{} {} @ {:#x}",
                                     k, static_cast<u32>(binding), cache_image.info.size.width,
                                     cache_image.info.size.height, vk::to_string(cache_image.info.pixel_format),
                                     cache_image.info.guest_address,
                                     scheduler.CurrentTick() - cache_image.tick_accessed_last, mip, slice,
                                     merged_image.info.size.width, merged_image.info.size.height,
                                     vk::to_string(merged_image.info.pixel_format), merged_image.info.guest_address);
                        }
                    }
                    VideoCore::Perf::tc_left_copy.fetch_add(1, std::memory_order_relaxed);
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    VideoCore::Perf::NoteFree(VideoCore::Perf::FreeLeftCopy);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

static bool MipMemoEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("NHLPORT_MIP_MEMO");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

void TextureCache::RememberMips(const ImageInfo& info) {
    if (!MipMemoEnabled() || info.resources.levels <= 1 || info.resources.layers != 1 ||
        info.type != AmdGpu::ImageType::Color2D) {
        return;
    }
    auto& levels = mip_memo[MipMemoKey{info.guest_address, info.size.width, info.size.height,
                                       info.pixel_format}];
    levels = std::max(levels, static_cast<u32>(info.resources.levels));
}

void TextureCache::ApplyRememberedMips(ImageInfo& info) const {
    if (!MipMemoEnabled() || info.resources.layers != 1 || info.type != AmdGpu::ImageType::Color2D) {
        return;
    }
    const auto it = mip_memo.find(
        MipMemoKey{info.guest_address, info.size.width, info.size.height, info.pixel_format});
    if (it == mip_memo.end() || it->second <= info.resources.levels) {
        return;
    }
    info.resources.levels = it->second;
    info.UpdateSize();
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    VideoCore::Perf::tc_expand.fetch_add(1, std::memory_order_relaxed);
    {
        static std::atomic<u64> n{0};
        const u64 k = n.fetch_add(1, std::memory_order_relaxed);
        const auto& old = slot_images[image_id].info;
        if (k < 60 || k % 5000 == 0) {
            LOG_INFO(Render, "expand #{}: {}x{} {} mips {} layers {} -> {}x{} {} mips {} layers {} @ {:#x}", k,
                     old.size.width, old.size.height, vk::to_string(old.pixel_format), old.resources.levels,
                     old.resources.layers, info.size.width, info.size.height, vk::to_string(info.pixel_format),
                     info.resources.levels, info.resources.layers, info.guest_address);
        }
    }
    RememberMips(info);
    ImageInfo expanded_info = info;
    if (slot_images[image_id].info.resources.levels < info.resources.levels &&
        slot_images[image_id].info.resources.layers == info.resources.layers) {
        ApplyRememberedMips(expanded_info);
    }
    const auto new_image_id = slot_images.Insert(instance, runtime, slot_image_views, expanded_info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    runtime.CopyImage(&src_image, &new_image);
    ApplyTextureOverride(new_image);
    DumpImageToDisk(new_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    VideoCore::Perf::NoteFree(VideoCore::Perf::FreeExpand);
    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    SmallVector<ImageId, 8> image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        if (image_id) {
            // nhlport: two cached images match the same address and size (aliases that were
            // never freed); the last one wins. Count and sample these ambiguous picks.
            VideoCore::Perf::tc_alias_pick.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<u64> n{0};
            const u64 k = n.fetch_add(1, std::memory_order_relaxed);
            if (k < 60 || k % 2000 == 0) {
                const auto& a = slot_images[image_id].info;
                LOG_INFO(Render,
                         "alias pick #{}: binding {} {}x{} {} @ {:#x}: candidates {} (mips {} idle {}) and {} "
                         "(mips {} idle {}), using the latter",
                         k, static_cast<u32>(desc.type), info.size.width, info.size.height,
                         vk::to_string(info.pixel_format), info.guest_address,
                         vk::to_string(a.pixel_format), a.resources.levels,
                         scheduler.CurrentTick() - slot_images[image_id].tick_accessed_last,
                         vk::to_string(cache_image.info.pixel_format), cache_image.info.resources.levels,
                         scheduler.CurrentTick() - cache_image.tick_accessed_last);
            }
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            VideoCore::Perf::NoteFree(VideoCore::Perf::FreeResolveFailed);
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        RememberMips(info);
        ImageInfo new_info = info;
        if (desc.type == BindingType::Texture || desc.type == BindingType::Storage) {
            ApplyRememberedMips(new_info);
        }
        image_id = slot_images.Insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(image_id);
    }

    VideoCore::Perf::current_request = nullptr;
    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    SmallVector<ImageId, 4> image_ids;
    ForEachImageWithAddress(address, [&](ImageId image_id, Image& image) {
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        }
    }
    UpdateImage(image_id);
    RefreshPortraitOnBind(image);
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        LinkMeta(image, MetaType::CMask, desc.info.meta_info.cmask_addr);
    }

    if (desc.info.meta_info.fmask_addr) {
        LinkMeta(image, MetaType::FMask, desc.info.meta_info.fmask_addr);
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        LinkMeta(image, MetaType::HTile, desc.info.meta_info.htile_addr,
                 image.info.meta_info.htile_clear_mask);
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(
            desc.info.stencil_addr, desc.info.stencil_size, [&](ImageId image_id, Image& image) {
                if (image.info.guest_address != desc.info.stencil_addr) {
                    return;
                }
                if (image.info.pixel_format == vk::Format::eUndefined ||
                    Vulkan::LiverpoolToVK::IsFormatStencilCompatible(image.info.pixel_format)) {
                    stencil_id = image_id;
                }
            });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.Insert(instance, runtime, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::DumpImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto& info = image.info;
    const bool is_depth = !(image.aspect_mask & vk::ImageAspectFlagBits::eColor);
    const u32 width = info.size.width;
    const u32 height = info.size.height;
    const auto block = vk::blockExtent(info.pixel_format);
    const u32 bytes = is_depth ? width * height * 4
                               : (width / block[0]) * (height / block[1]) *
                                     vk::blockSize(info.pixel_format);
    auto& staging_pool = runtime.GetStagingPool();
    const auto staging =
        staging_pool.Request(bytes, VideoCore::MemoryType::HostCached, instance.StorageMinAlignment());
    const vk::BufferImageCopy copy = {
        .bufferOffset = staging.offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = is_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    const std::array copies{copy};
    runtime.DownloadImage(&image, staging.buffer, copies);
    scheduler.Finish();
    staging.buffer->Invalidate(staging.offset, bytes);
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::CapturesDir) /
                      fmt::format("img_{:#x}_{}x{}_{}_f{}.raw", info.guest_address, width, height,
                                  is_depth ? std::string("Depth32") : vk::to_string(info.pixel_format),
                                  VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed));
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Create};
    file.WriteSpan(std::span<const u8>{staging.mapped, bytes});
    LOG_WARNING(Render, "image dump: id {} {}x{} {} @ {:#x} -> {} ({} bytes)", image_id.index, width,
                height, vk::to_string(info.pixel_format), info.guest_address,
                path.string(), bytes);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    if ((is_gpu_modified || image.usage.render_target) &&
        VideoCore::Perf::draw_trace.load(std::memory_order_relaxed)) {
        VideoCore::Perf::DrawLog(fmt::format(
            "refresh f{} d{} id {} {}x{} {} @ {:#x} rt {} gpumod {} gpudirty {} cpudirty {} maybe {} src {}",
            VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed),
            VideoCore::Perf::draw_index.load(std::memory_order_relaxed), slot_images.GetSlotId(image).index,
            image.info.size.width, image.info.size.height, vk::to_string(image.info.pixel_format),
            image.info.guest_address, u32(image.usage.render_target), u32(is_gpu_modified), u32(is_gpu_dirty),
            u32(True(image.flags & ImageFlagBits::CpuDirty)),
            u32(True(image.flags & ImageFlagBits::MaybeCpuDirty)),
            buffer_cache.IsRegionGpuModified(image.info.guest_address, image.info.guest_size) ? "gpu-buffer"
                                                                                              : "guest-mem"));
    }

    SmallVector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];
        const u32 extent_width = mip_pitch ? std::min<u32>(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min<u32>(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    Diag::C::tc_refresh.fetch_add(1, std::memory_order_relaxed);
    Diag::C::tc_refresh_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    runtime.UploadImage(&image, buffer, image_copies);
    ApplyPortraitOverride(image);
    ApplyTextureOverride(image);
    DumpImageToDisk(image);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sharp,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth) {
    // Compare and plain uses of one S# need separate samplers.
    const u64 hash = HashCombine(XXH3_64bits(&sharp, sizeof(sharp)), is_depth);
    const auto [it, new_sampler] = samplers.try_emplace(hash);
    if (new_sampler) {
        it->second = slot_samplers.Insert(instance, sharp, border_color_base, is_depth);
        Sampler& sampler = slot_samplers[it->second];
        sampler.hash = hash;
        sampler_lru_cache.Insert(sampler, gc_tick);
    }

    Sampler& sampler = slot_samplers[it->second];
    sampler_lru_cache.Touch(sampler, gc_tick);
    return sampler.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    VideoCore::Perf::tc_images.fetch_add(1, std::memory_order_relaxed);
    VideoCore::Perf::tc_created.fetch_add(1, std::memory_order_relaxed);
    if (VideoCore::Perf::draw_trace.load(std::memory_order_relaxed)) {
        const auto& i = image.info;
        VideoCore::Perf::DrawLog(fmt::format("tc+ f{} id {} {}x{}x{} {} mips {} layers {} depth {} @ {:#x} size {:#x} tile {:#x}",
                 VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed), image_id.index,
                 i.size.width, i.size.height, i.size.depth, vk::to_string(i.pixel_format),
                 i.resources.levels, i.resources.layers, u32(i.props.is_depth), i.guest_address,
                 i.guest_size, static_cast<u32>(i.tile_mode)));
    }
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image_lru_cache.Insert(image, gc_tick);
    const auto& info = image.info;
    ASSERT_MSG((info.guest_address & 0xff) == 0, "Trying to register an unaligned image");
    ForEachPage(info.guest_address, info.guest_size, [this, image_id, info](u64 page) {
        page_table[page].entries.emplace_back(BucketEntry{
            .key = u32(info.guest_address >> 8),
            .size = info.guest_size,
            .id = image_id,
        });
    });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    VideoCore::Perf::tc_images.fetch_sub(1, std::memory_order_relaxed);
    if (VideoCore::Perf::draw_trace.load(std::memory_order_relaxed)) {
        const auto& i = image.info;
        const auto* req = static_cast<const ImageInfo*>(VideoCore::Perf::current_request);
        const std::string req_str =
            req ? fmt::format(" req {}x{} {} mips {} @ {:#x} size {:#x} tile {:#x} bind {}", req->size.width,
                              req->size.height, vk::to_string(req->pixel_format),
                              req->resources.levels, req->guest_address, req->guest_size,
                              static_cast<u32>(req->tile_mode), VideoCore::Perf::current_binding)
                : std::string{};
        VideoCore::Perf::DrawLog(fmt::format("tc- f{} id {} reason {} {}x{} {} mips {} layers {} depth {} @ {:#x} size {:#x} tile {:#x} gpumod {} idle {}{}",
                 VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed), image_id.index,
                 VideoCore::Perf::last_free_reason, i.size.width, i.size.height,
                 vk::to_string(i.pixel_format), i.resources.levels, i.resources.layers,
                 u32(i.props.is_depth), i.guest_address, i.guest_size, static_cast<u32>(i.tile_mode),
                 u32(True(image.flags & ImageFlagBits::GpuModified)),
                 scheduler.CurrentTick() - image.tick_accessed_last, req_str));
        VideoCore::Perf::last_free_reason = 0xffffffffu;
    }
    image_lru_cache.Free(image);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        ASSERT_MSG(page_it, "Unregistering unregistered page={:#x}", page << Traits::PAGE_BITS);
        auto& entries = page_it->entries;
        const auto vector_it = std::ranges::find(entries, image_id, &BucketEntry::id);
        ASSERT_MSG(vector_it != entries.end(), "Unregistering unregistered image in page={:#x}",
                   page << Traits::PAGE_BITS);
        entries.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::Registered) || image.streamed) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    std::scoped_lock lk{image.mutex};
    if (image.IsUntracked()) {
        tracker.UpdatePageWatchers(image_begin, image.info.guest_size, PageOp::Track);
    } else {
        if (image_begin < image.track_addr) {
            tracker.UpdatePageWatchers(image_begin, image.track_addr - image_begin, PageOp::Track);
        }
        if (image.track_addr_end < image_end) {
            tracker.UpdatePageWatchers(image.track_addr_end, image_end - image.track_addr_end,
                                       PageOp::Track);
        }
    }
    image.track_addr = image_begin;
    image.track_addr_end = image_end;
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (image.IsUntracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    if (size != 0) {
        tracker.UpdatePageWatchers(addr, size, PageOp::Untrack);
    }
    image.track_addr = 0;
    image.track_addr_end = 0;
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (image.IsUntracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    tracker.UpdatePageWatchers(image_begin, size, PageOp::Untrack);

    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image.IsUntracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers(addr, size, PageOp::Untrack);
}

void TextureCache::GarbageCollectImages() {
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](Image& image) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        const auto image_id = slot_images.GetSlotId(image);
        if (download) {
            DownloadImageMemory(image_id);
        }
        VideoCore::Perf::NoteFree(VideoCore::Perf::FreeGc);
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    image_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        image_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](Sampler& sampler) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        sampler_lru_cache.Free(sampler);
        samplers.erase(sampler.hash);
        const auto sampler_id = slot_samplers.GetSlotId(sampler);
        slot_samplers.Erase(sampler_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    GarbageCollectImages();
    GarbageCollectSamplers();
    ++gc_tick;
}

// nhlport: NHL 24 (Frostbite) hands the same CMASK / HTILE memory to several render targets
// that alias each other in its transient pool (e.g. the 1080p UI target and a scene-sized
// GBuffer target at one base address). Upstream erased the metadata entry whenever ANY image
// naming it was deleted and re-created it as "all slices cleared" on the next bind, so the
// surviving target was wiped by a spurious loadOp CLEAR in the middle of its pass (team-select
// menu: the UI backdrop vanished for one frame and the blurred arena showed through).
// NHLPORT_META_ERASE=1 restores the upstream behaviour for comparison.
static bool MetaEraseLegacy() {
    static const bool legacy = [] {
        const char* env = std::getenv("NHLPORT_META_ERASE");
        return env && env[0] == '1';
    }();
    return legacy;
}

VAddr& TextureCache::MetaLink(Image& image, MetaType type) {
    switch (type) {
    case MetaType::CMask:
        return image.meta_links.cmask;
    case MetaType::FMask:
        return image.meta_links.fmask;
    case MetaType::HTile:
    default:
        return image.meta_links.htile;
    }
}

void TextureCache::LinkMeta(Image& image, MetaType type, VAddr address, s32 initial_clear_mask) {
    VAddr& link = MetaLink(image, type);
    const bool new_link = link != address;
    if (new_link && link != 0) {
        // The image moved to another metadata surface: give the old reference back first.
        UnlinkMeta(image, type);
    }
    auto [it, inserted] =
        surface_metas.try_emplace(address, MetaDataInfo{.type = type, .clear_mask = initial_clear_mask});
    if (new_link) {
        // Counted through image.meta_links rather than info.meta_info: the latter already holds
        // the address for images created from the colour / depth buffer registers, which left
        // their first link uncounted (refs 0) and let the deletion of any one owner erase the
        // entry, exactly the upstream behaviour the refcount was meant to replace.
        link = address;
        ++it->second.refs;
    }
    if (inserted || new_link) {
        const auto& i = image.info;
        it->second.owner = fmt::format("{}x{} {} mips {} layers {} @ {:#x}{}", i.size.width,
                                       i.size.height, vk::to_string(i.pixel_format),
                                       i.resources.levels, i.resources.layers, i.guest_address,
                                       image.usage.depth_target ? " (depth)" : "");
    }
    // Mirror the address into the image info like upstream did (diagnostics only).
    switch (type) {
    case MetaType::CMask:
        image.info.meta_info.cmask_addr = address;
        break;
    case MetaType::FMask:
        image.info.meta_info.fmask_addr = address;
        break;
    case MetaType::HTile:
        image.info.meta_info.htile_addr = address;
        break;
    }
    if (VideoCore::Perf::draw_trace.load(std::memory_order_relaxed)) {
        const u64 frame = VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed);
        if (inserted) {
            VideoCore::Perf::DrawLog(fmt::format("meta-new f{} @ {:#x} type {} clear_mask {:#x}",
                                                 frame, address, static_cast<u32>(type),
                                                 static_cast<u32>(initial_clear_mask)));
        }
        if (new_link) {
            VideoCore::Perf::DrawLog(fmt::format("meta-link f{} @ {:#x} type {} refs {} image @ {:#x}",
                                                 frame, address, static_cast<u32>(type),
                                                 it->second.refs, image.info.guest_address));
        }
    }
}

void TextureCache::UnlinkMeta(Image& image, MetaType type) {
    VAddr& link = MetaLink(image, type);
    if (link == 0) {
        return;
    }
    const VAddr address = link;
    link = 0;
    UnlinkMeta(address);
}

void TextureCache::UnlinkMeta(VAddr address) {
    auto it = surface_metas.find(address);
    if (it == surface_metas.end()) {
        return;
    }
    if (it->second.refs > 0) {
        --it->second.refs;
    }
    const bool erase = MetaEraseLegacy() || it->second.refs == 0;
    if (VideoCore::Perf::draw_trace.load(std::memory_order_relaxed)) {
        VideoCore::Perf::DrawLog(fmt::format("meta-unlink f{} @ {:#x} refs {} clear_mask {:#x} erase {}",
                                             VideoCore::Perf::gpu_frame.load(std::memory_order_relaxed),
                                             address, it->second.refs,
                                             static_cast<u32>(it->second.clear_mask), u32(erase)));
    }
    if (erase) {
        surface_metas.erase(it);
    }
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(image.IsUntracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Give back the references this image holds on its metadata surfaces; the entry is erased
    // with the last owner (see LinkMeta). Images that name a metadata address in info.meta_info
    // but were never bound as a target hold no reference and must not touch the entry.
    UnlinkMeta(image, MetaType::CMask);
    UnlinkMeta(image, MetaType::FMask);
    UnlinkMeta(image, MetaType::HTile);

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.Erase(image_view_id);
            }
        }
        slot_images.Erase(image_id);
    });
}

} // namespace VideoCore
