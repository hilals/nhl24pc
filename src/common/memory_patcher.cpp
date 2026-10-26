// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <codecvt>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_state.h"
#include "core/file_format/psf.h"
#include "memory_patcher.h"
#include "common/types.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace MemoryPatcher {

EXPORT uintptr_t g_eboot_address;
uint64_t g_eboot_image_size;
std::string g_eboot_name;
std::string g_game_serial;
std::string patch_file;
bool patches_applied = false;
std::vector<patchInfo> pending_patches;

std::string toHex(u64 value, size_t byteSize) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(byteSize * 2) << value;
    return ss.str();
}

static bool isHexSym(const std::string& s) {
    return (s.size() >= 2 && (s[0] == '$' || s[0] == '#'));
}

static bool isHex0x(const std::string& s) {
    return (s.size() >= 3 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
}

// possible prefixes == (s == `0x` || s == `#` || s == `$`) will be interpet as hex value, else is
// decimal
static int convertNumBase(const std::string& s) {
    return (isHex0x(s) || isHexSym(s)) ? 16 : 10;
}

std::string convertValueToHex(const std::string type, const std::string valueStr) {
    std::string result;

    if (type == "byte") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 1);
    } else if (type == "bytes16") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 2);
    } else if (type == "bytes32") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 4);
    } else if (type == "bytes64") {
        const u64 value = std::stoull(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 8);
    } else if (type == "float32") {
        union {
            float f;
            uint32_t i;
        } floatUnion;
        floatUnion.f = std::stof(valueStr);
        result = toHex(std::byteswap(floatUnion.i), sizeof(floatUnion.i));
    } else if (type == "float64") {
        union {
            double d;
            uint64_t i;
        } doubleUnion;
        doubleUnion.d = std::stod(valueStr);
        result = toHex(std::byteswap(doubleUnion.i), sizeof(doubleUnion.i));
    } else if (type == "utf8") {
        std::vector<unsigned char> byteArray =
            std::vector<unsigned char>(valueStr.begin(), valueStr.end());
        byteArray.push_back('\0');
        std::stringstream ss;
        for (unsigned char c : byteArray) {
            ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
        result = ss.str();
    } else if (type == "utf16") {
        std::wstring wide_str(valueStr.size(), L'\0');
        std::mbstowcs(&wide_str[0], valueStr.c_str(), valueStr.size());
        wide_str.resize(std::wcslen(wide_str.c_str()));

        std::u16string valueStringU16;

        for (wchar_t wc : wide_str) {
            if (wc <= 0xFFFF) {
                valueStringU16.push_back(static_cast<char16_t>(wc));
            } else {
                wc -= 0x10000;
                valueStringU16.push_back(static_cast<char16_t>(0xD800 | (wc >> 10)));
                valueStringU16.push_back(static_cast<char16_t>(0xDC00 | (wc & 0x3FF)));
            }
        }

        std::vector<unsigned char> byteArray;
        // convert to little endian
        for (char16_t ch : valueStringU16) {
            unsigned char low_byte = static_cast<unsigned char>(ch & 0x00FF);
            unsigned char high_byte = static_cast<unsigned char>((ch >> 8) & 0x00FF);

            byteArray.push_back(low_byte);
            byteArray.push_back(high_byte);
        }
        byteArray.push_back('\0');
        byteArray.push_back('\0');
        std::stringstream ss;

        for (unsigned char ch : byteArray) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
        result = ss.str();
    } else if (type == "bytes") {
        result = valueStr;
    } else if (type == "mask" || type == "mask_jump32") {
        result = valueStr;
    } else {
        LOG_INFO(Loader, "Error applying Patch, unknown type: {}", type);
    }
    return result;
}

void ApplyPendingPatches();

void ApplyPatchesFromXML(std::filesystem::path path) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());

    auto* param_sfo = Common::Singleton<PSF>::Instance();
    auto app_version = param_sfo->GetString("APP_VER").value_or("Unknown version");

    if (result) {
        auto patchXML = doc.child("Patch");
        for (pugi::xml_node_iterator it = patchXML.children().begin();
             it != patchXML.children().end(); ++it) {

            if (std::string(it->name()) == "Metadata") {
                if (std::string(it->attribute("isEnabled").value()) == "true") {
                    std::string currentPatchName = it->attribute("Name").value();
                    std::string metadataAppVer = it->attribute("AppVer").value();
                    std::string metadataAppElf = it->attribute("AppElf").value();
                    bool versionMatches = metadataAppVer == app_version;

                    if (metadataAppElf != g_eboot_name)
                        continue;

                    auto patchList = it->first_child();
                    for (pugi::xml_node_iterator patchLineIt = patchList.children().begin();
                         patchLineIt != patchList.children().end(); ++patchLineIt) {

                        std::string type = patchLineIt->attribute("Type").value();
                        if (!versionMatches && type != "mask" && type != "mask_jump32")
                            continue;

                        std::string address = patchLineIt->attribute("Address").value();
                        std::string patchValue = patchLineIt->attribute("Value").value();
                        std::string maskOffsetStr = patchLineIt->attribute("Offset").value();
                        std::string targetStr = "";
                        std::string sizeStr = "";
                        if (type == "mask_jump32") {
                            targetStr = patchLineIt->attribute("Target").value();
                            sizeStr = patchLineIt->attribute("Size").value();
                        } else {
                            try {
                                patchValue = convertValueToHex(type, patchValue);
                            } catch (std::exception& e) {
                                ASSERT_MSG(false,
                                           "Failed to parse patch value \"{}\" for \"{}\" in "
                                           "patch \"{}\", error: \"{}\"\n"
                                           "If the patch was working on earlier versions, then it "
                                           "was using a format that shadPS4 handled incorrectly, "
                                           "and the patch should instead be fixed.",
                                           patchValue, address, currentPatchName, e.what());
                            }
                        }

                        bool littleEndian = false;
                        if (type == "bytes16" || type == "bytes32" || type == "bytes64") {
                            littleEndian = true;
                        }

                        MemoryPatcher::PatchMask patchMask = MemoryPatcher::PatchMask::None;
                        int maskOffsetValue = 0;

                        if (type == "mask")
                            patchMask = MemoryPatcher::PatchMask::Mask;

                        if (type == "mask_jump32")
                            patchMask = MemoryPatcher::PatchMask::Mask_Jump32;

                        if ((type == "mask" || type == "mask_jump32") && !maskOffsetStr.empty()) {
                            maskOffsetValue = std::stoi(maskOffsetStr, 0, 10);
                        }

                        const patchInfo patch = {
                            .gameSerial = "*",
                            .modNameStr = currentPatchName,
                            .offsetStr = address,
                            .valueStr = patchValue,
                            .targetStr = targetStr,
                            .sizeStr = sizeStr,
                            .littleEndian = littleEndian,
                            .patchMask = patchMask,
                            .maskOffset = maskOffsetValue,
                        };
                        MemoryPatcher::PatchMemory(patch);
                    }
                }
            }
        }
    } else {
        LOG_ERROR(Loader, "Could not parse patch XML: {}", result.description());
    }
}

typedef void (PS4_SYSV_ABI *UIInsertFn)(void* map, const char* key, const char* val);
static UIInsertFn s_orig_ui_insert = nullptr;

static inline bool StrEqualsIgnoreCase(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + ('a' - 'A')) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + ('a' - 'A')) : *b;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static void PS4_SYSV_ABI Hook_UIInsert(void* map, const char* key, const char* val) {
    if (key != nullptr && reinterpret_cast<uintptr_t>(key) >= 0x10000) {
        const char* orig_val = val;
        if (StrEqualsIgnoreCase(key, "TeamCity") || StrEqualsIgnoreCase(key, "CityName") ||
            StrEqualsIgnoreCase(key, "CategoryName") || StrEqualsIgnoreCase(key, "FranchiseCategory") ||
            StrEqualsIgnoreCase(key, "City") || StrEqualsIgnoreCase(key, "TeamCityName") ||
            StrEqualsIgnoreCase(key, "TeamNameLine1") || StrEqualsIgnoreCase(key, "UserTeamNameLine1") ||
            StrEqualsIgnoreCase(key, "FranchiseTeamNameLine1") || StrEqualsIgnoreCase(key, "OpponentTeamNameLine1") ||
            StrEqualsIgnoreCase(key, "NameLine1") || StrEqualsIgnoreCase(key, "Line1Name") ||
            StrEqualsIgnoreCase(key, "Team_Line1Name") || StrEqualsIgnoreCase(key, "Line1")) {
            if (!val || val[0] == '\0' || StrEqualsIgnoreCase(val, "Arizona")) {
                val = "Utah";
            }
        } else if (StrEqualsIgnoreCase(key, "TeamFullName") || StrEqualsIgnoreCase(key, "FullName")) {
            if (!val || val[0] == '\0' || StrEqualsIgnoreCase(val, "Arizona Coyotes")) {
                val = "Utah Mammoth";
            }
        } else if (StrEqualsIgnoreCase(key, "TeamName") || StrEqualsIgnoreCase(key, "TeamNameLine2") ||
                   StrEqualsIgnoreCase(key, "UserTeamNameLine2") || StrEqualsIgnoreCase(key, "FranchiseTeamNameLine2") ||
                   StrEqualsIgnoreCase(key, "OpponentTeamNameLine2") || StrEqualsIgnoreCase(key, "NameLine2") ||
                   StrEqualsIgnoreCase(key, "Line2Name") || StrEqualsIgnoreCase(key, "Team_Line2Name") ||
                   StrEqualsIgnoreCase(key, "teamLine2Name") || StrEqualsIgnoreCase(key, "Line2") ||
                   StrEqualsIgnoreCase(key, "TeamNickname") || StrEqualsIgnoreCase(key, "NickName") ||
                   StrEqualsIgnoreCase(key, "Nickname")) {
            if (!val || val[0] == '\0' || StrEqualsIgnoreCase(val, "Coyotes")) {
                val = "Mammoth";
            }
        } else if (StrEqualsIgnoreCase(key, "TeamAbbrName") || StrEqualsIgnoreCase(key, "AbbrName") ||
                   StrEqualsIgnoreCase(key, "TeamAbbreviation") || StrEqualsIgnoreCase(key, "TeamAbbr") ||
                   StrEqualsIgnoreCase(key, "Abbreviation")) {
            if (!val || val[0] == '\0' || StrEqualsIgnoreCase(val, "PHX") ||
                StrEqualsIgnoreCase(val, "ARZ") || StrEqualsIgnoreCase(val, "ARI")) {
                val = "UTA";
            }
        } else if (StrEqualsIgnoreCase(key, "TileName")) {
            if (StrEqualsIgnoreCase(val, "Arizona")) {
                val = "Utah";
            }
        } else if (StrEqualsIgnoreCase(key, "ArenaName") || StrEqualsIgnoreCase(key, "StadiumName") ||
                   StrEqualsIgnoreCase(key, "VenueName") || StrEqualsIgnoreCase(key, "Arena") ||
                   StrEqualsIgnoreCase(key, "Stadium") || StrEqualsIgnoreCase(key, "Venue")) {
            if (!val || val[0] == '\0' || StrEqualsIgnoreCase(val, "Arizona Arena")) {
                val = "Delta Center";
            }
        } else if (StrEqualsIgnoreCase(key, "ArenaCity") || StrEqualsIgnoreCase(key, "StadiumCity") ||
                   StrEqualsIgnoreCase(key, "VenueCity") || StrEqualsIgnoreCase(key, "ArenaLocation") ||
                   StrEqualsIgnoreCase(key, "StadiumLocation")) {
            if (!val || val[0] == '\0' || StrEqualsIgnoreCase(val, "Tempe, AZ") || StrEqualsIgnoreCase(val, "Tempe")) {
                val = "Delta Center, UT";
            }
        }

        if (val != nullptr && reinterpret_cast<uintptr_t>(val) >= 0x10000) {
            if (StrEqualsIgnoreCase(val, "Arizona Arena")) {
                val = "Delta Center";
            } else if (StrEqualsIgnoreCase(val, "Tempe, AZ")) {
                val = "Delta Center, UT";
            } else if (StrEqualsIgnoreCase(val, "ARIZONA ARENA TEMPE, AZ") ||
                       StrEqualsIgnoreCase(val, "ARIZONA ARENA  TEMPE, AZ") ||
                       StrEqualsIgnoreCase(val, "Arizona Arena, Tempe, AZ")) {
                val = "DELTA CENTER, UT";
            }
        }
        if (val != orig_val) {
            LOG_INFO(Loader, "UIInsert override: key='{}', val='{}' -> '{}'",
                     key, orig_val ? orig_val : "<null>", val);
        }
    }
    s_orig_ui_insert(map, key, val);
}

static void InstallUtahBrandingHook() {
#ifdef _WIN32
    if (g_game_serial != "CUSA37933" && !g_game_serial.empty()) {
        return;
    }
    if (g_eboot_address == 0) {
        return;
    }

    uintptr_t target = g_eboot_address + 0x256cab0;
    static const uint8_t expected_prologue[17] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28
    };

    if (std::memcmp(reinterpret_cast<const void*>(target), expected_prologue, sizeof(expected_prologue)) != 0) {
        LOG_WARNING(Loader, "Utah branding hook: prologue mismatch at {:#x}, skipping hook", target);
        return;
    }

    // Allocate executable trampoline buffer
    uint8_t* trampoline = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    );
    if (!trampoline) {
        LOG_ERROR(Loader, "Utah branding hook: failed to allocate trampoline memory");
        return;
    }

    // Copy stolen 17 bytes to trampoline
    std::memcpy(trampoline, reinterpret_cast<const void*>(target), 17);

    // Append 14-byte 64-bit indirect jump back to target + 17
    trampoline[17] = 0xFF;
    trampoline[18] = 0x25;
    trampoline[19] = 0x00;
    trampoline[20] = 0x00;
    trampoline[21] = 0x00;
    trampoline[22] = 0x00;
    uint64_t ret_addr = static_cast<uint64_t>(target + 17);
    std::memcpy(&trampoline[23], &ret_addr, sizeof(ret_addr));

    FlushInstructionCache(GetCurrentProcess(), trampoline, 31);
    s_orig_ui_insert = reinterpret_cast<UIInsertFn>(trampoline);

    // Overwrite target with 14-byte 64-bit jump to Hook_UIInsert + 3 NOPs
    DWORD old_protect;
    if (!VirtualProtect(reinterpret_cast<void*>(target), 17, PAGE_EXECUTE_READWRITE, &old_protect)) {
        LOG_ERROR(Loader, "Utah branding hook: VirtualProtect failed at target");
        return;
    }

    uint8_t jump_code[17];
    jump_code[0] = 0xFF;
    jump_code[1] = 0x25;
    jump_code[2] = 0x00;
    jump_code[3] = 0x00;
    jump_code[4] = 0x00;
    jump_code[5] = 0x00;
    uint64_t hook_addr = reinterpret_cast<uint64_t>(&Hook_UIInsert);
    std::memcpy(&jump_code[6], &hook_addr, sizeof(hook_addr));
    jump_code[14] = 0x90;
    jump_code[15] = 0x90;
    jump_code[16] = 0x90;

    std::memcpy(reinterpret_cast<void*>(target), jump_code, sizeof(jump_code));
    VirtualProtect(reinterpret_cast<void*>(target), 17, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 17);

    LOG_INFO(Loader, "Utah branding hook successfully installed at target {:#x} -> trampoline {:#x}",
             target, reinterpret_cast<uintptr_t>(trampoline));
#endif
}

void OnGameLoaded() {
    std::filesystem::path patch_dir = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir);
    if (!patch_file.empty()) {

        auto file_path = (patch_dir / patch_file).native();
        if (std::filesystem::exists(patch_file)) {
            ApplyPatchesFromXML(patch_file);
        } else {
            ApplyPatchesFromXML(file_path);
        }
    } else if (EmulatorState::GetInstance()->IsAutoPatchesLoadEnabled()) {
        for (auto const& repo : std::filesystem::directory_iterator(patch_dir)) {
            if (!repo.is_directory()) {
                continue;
            }

            const auto files_json_path = repo.path() / "files.json";

            if (!std::filesystem::is_regular_file(files_json_path)) {
                LOG_ERROR(Loader, "The folder '{}' does not contain the files.json file.",
                          repo.path().filename().string());
                continue;
            }

            std::ifstream json_file{files_json_path};
            nlohmann::json available_patches;

            if (!json_file.is_open()) {
                LOG_ERROR(Loader, "Could not open patch index: {}", files_json_path.string());
                continue;
            }

            try {
                available_patches = nlohmann::json::parse(json_file);
            } catch (const nlohmann::json::parse_error& e) {
                LOG_ERROR(Loader, "Could not parse patch index {}: {}", files_json_path.string(),
                          e.what());
                continue;
            }

            std::filesystem::path game_patch_file;
            for (auto const& [filename, serials] : available_patches.items()) {
                if (std::find(serials.begin(), serials.end(), g_game_serial) != serials.end()) {
                    game_patch_file = repo.path() / filename;
                    break;
                }
            }
            if (std::filesystem::exists(game_patch_file)) {
                ApplyPatchesFromXML(game_patch_file);
            }
        }
    }
    ApplyPendingPatches();
    InstallUtahBrandingHook();
}

void AddPatchToQueue(const patchInfo& patchToAdd) {
    if (patches_applied) {
        PatchMemory(patchToAdd);
        return;
    }
    pending_patches.push_back(patchToAdd);
}

void ApplyPendingPatches() {
    patches_applied = true;
    for (size_t i = 0; i < pending_patches.size(); ++i) {
        const patchInfo& currentPatch = pending_patches[i];

        if (currentPatch.gameSerial != "*" && currentPatch.gameSerial != g_game_serial)
            continue;

        PatchMemory(currentPatch);
    }

    pending_patches.clear();
}

void PatchMemory(const patchInfo& patch) {
    // Send a request to modify the process memory.
    void* cheatAddress = nullptr;

    if (patch.patchMask == PatchMask::None) {
        if (patch.isOffset) {
            cheatAddress =
                reinterpret_cast<void*>(g_eboot_address + std::stoi(patch.offsetStr, 0, 16));
        } else {
            cheatAddress = reinterpret_cast<void*>(g_eboot_address +
                                                   (std::stoi(patch.offsetStr, 0, 16) - 0x400000));
        }
    }

    if (patch.patchMask == PatchMask::Mask) {
        cheatAddress = reinterpret_cast<void*>(PatternScan(patch.offsetStr) + patch.maskOffset);
    }

    if (patch.patchMask == PatchMask::Mask_Jump32) {
        int jumpSize = std::stoi(patch.sizeStr);

        constexpr int MAX_PATTERN_LENGTH = 256;
        if (jumpSize < 5) {
            LOG_ERROR(Loader, "Jump size must be at least 5 bytes");
            return;
        }
        if (jumpSize > MAX_PATTERN_LENGTH) {
            LOG_ERROR(Loader, "Jump size must be no more than {} bytes.", MAX_PATTERN_LENGTH);
            return;
        }

        // Find the base address using "Address"
        uintptr_t baseAddress = PatternScan(patch.offsetStr);
        if (baseAddress == 0) {
            LOG_ERROR(Loader, "PatternScan failed for mask_jump32 with pattern: {}",
                      patch.offsetStr);
            return;
        }
        uintptr_t patchAddress = baseAddress + patch.maskOffset;

        // Fills the original region (jumpSize bytes) with NOPs
        std::vector<u8> nopBytes(jumpSize, 0x90);
        std::memcpy(reinterpret_cast<void*>(patchAddress), nopBytes.data(), nopBytes.size());

        // Use "Target" to locate the start of the code cave
        uintptr_t jump_target = PatternScan(patch.targetStr);
        if (jump_target == 0) {
            LOG_ERROR(Loader, "PatternScan failed to Target with pattern: {}", patch.targetStr);
            return;
        }

        // Converts the Value attribute to a byte array (payload)
        std::vector<u8> payload;
        for (size_t i = 0; i < patch.valueStr.length(); i += 2) {
            std::string tempStr = patch.valueStr.substr(i, 2);
            const char* byteStr = tempStr.c_str();
            char* endPtr;
            unsigned int byteVal = std::strtoul(byteStr, &endPtr, 16);

            if (endPtr != byteStr + 2) {
                LOG_ERROR(Loader, "Invalid byte in Value: {}", patch.valueStr.substr(i, 2));
                return;
            }
            payload.push_back(static_cast<u8>(byteVal));
        }

        // Calculates the end of the code cave (where the return jump will be inserted)
        uintptr_t code_cave_end = jump_target + payload.size();

        // Write the payload to the code cave, from jump_target
        std::memcpy(reinterpret_cast<void*>(jump_target), payload.data(), payload.size());

        // Inserts the initial jump in the original region to divert to the code cave
        u8 jumpInstruction[5];
        jumpInstruction[0] = 0xE9;
        s32 relJump = static_cast<s32>(jump_target - patchAddress - 5);
        std::memcpy(&jumpInstruction[1], &relJump, sizeof(relJump));
        std::memcpy(reinterpret_cast<void*>(patchAddress), jumpInstruction,
                    sizeof(jumpInstruction));

        // Inserts jump back at the end of the code cave to resume execution after patching
        u8 jumpBack[5];
        jumpBack[0] = 0xE9;
        // Calculates the relative offset to return to the instruction immediately following the
        // overwritten region
        s32 target_return = static_cast<s32>((patchAddress + jumpSize) - (code_cave_end + 5));
        std::memcpy(&jumpBack[1], &target_return, sizeof(target_return));
        std::memcpy(reinterpret_cast<void*>(code_cave_end), jumpBack, sizeof(jumpBack));

        LOG_INFO(Loader,
                 "Applied Patch mask_jump32: {}, PatchAddress: {:#x}, JumpTarget: {:#x}, "
                 "CodeCaveEnd: {:#x}, JumpSize: {}",
                 patch.modNameStr, patchAddress, jump_target, code_cave_end, jumpSize);
        return;
    }

    if (cheatAddress == nullptr) {
        LOG_ERROR(Loader, "Failed to get address for patch {}", patch.modNameStr);
        return;
    }

    std::vector<unsigned char> bytePatch;

    for (size_t i = 0; i < patch.valueStr.length(); i += 2) {
        unsigned char byte = static_cast<unsigned char>(
            std::strtol(patch.valueStr.substr(i, 2).c_str(), nullptr, 16));

        bytePatch.push_back(byte);
    }

    if (patch.littleEndian) {
        std::reverse(bytePatch.begin(), bytePatch.end());
    }

    std::memcpy(cheatAddress, bytePatch.data(), bytePatch.size());

    LOG_INFO(Loader, "Applied patch: {}, Offset: {:#x}, Value: {}", patch.modNameStr,
             (uintptr_t)cheatAddress, patch.valueStr);
}

static std::vector<int32_t> PatternToByte(const std::string& pattern) {
    std::vector<int32_t> bytes;
    const char* start = pattern.data();
    const char* end = start + pattern.size();

    for (const char* current = start; current < end; ++current) {
        if (*current == '?') {
            ++current;
            if (*current == '?')
                ++current;
            bytes.push_back(-1);
        } else {
            bytes.push_back(strtoul(current, const_cast<char**>(&current), 16));
        }
    }

    return bytes;
}

uintptr_t PatternScan(const std::string& signature) {
    std::vector<int32_t> patternBytes = PatternToByte(signature);
    const auto scanBytes = static_cast<uint8_t*>((void*)g_eboot_address);

    const int32_t* sigPtr = patternBytes.data();
    const size_t sigSize = patternBytes.size();

    uint32_t foundResults = 0;
    for (uint32_t i = 0; i < g_eboot_image_size - sigSize; ++i) {
        bool found = true;
        for (uint32_t j = 0; j < sigSize; ++j) {
            if (scanBytes[i + j] != sigPtr[j] && sigPtr[j] != -1) {
                found = false;
                break;
            }
        }

        if (found) {
            foundResults++;
            return reinterpret_cast<uintptr_t>(&scanBytes[i]);
        }
    }

    return 0;
}

} // namespace MemoryPatcher
