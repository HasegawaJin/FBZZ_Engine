/// @file    AssetBrowserVisuals.cpp
/// @brief   Original bitmap type icons, explicit preview states and bounded cache lifetime.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "AssetBrowserCommon.hpp"
#include <Editor/Panels/AssetBrowserThumbnail.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <string>

namespace fbzz::editor {

void AssetBrowserPanel::TouchAssetPreview(const std::string& path)
{
    if (!m_previewAccess.contains(path) && m_previewAccess.size() >= assetthumbnail::MAX_CACHE_ENTRIES) {
        const auto oldest = std::min_element(m_previewAccess.begin(), m_previewAccess.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; });
        const std::string key = oldest->first;
        ResetSingleAssetPreviewCache(key);
    }
    m_previewAccess[path] = ++m_previewAccessSerial;
}

void AssetBrowserPanel::ResetAssetPreviewCache(const std::string& path)
{
    std::vector<std::string> affected;
    const std::string normalized = util::FileSystem::NormalizePathSeparators(path);
    for (const auto& [key, serial] : m_previewAccess) {
        const std::string candidate = util::FileSystem::NormalizePathSeparators(key);
        if (util::FileSystem::SamePathText(candidate, normalized) ||
            util::FileSystem::IsChildPathText(candidate, normalized) || candidate.starts_with(normalized + "::"))
            affected.push_back(key);
    }
    for (const auto& key : affected) ResetSingleAssetPreviewCache(key);
    ResetSingleAssetPreviewCache(path);
    /// @note Cached Prefab/VFX/Terrain images depend on other assets; re-evaluate on dependency changes.
    auto releaseDependent = [&](auto& cache) {
        std::vector<std::string> keys;
        for (const auto& item : cache) keys.push_back(item.first);
        for (const auto& key : keys) ResetSingleAssetPreviewCache(key);
    };
    releaseDependent(m_prefabPreviews);
    releaseDependent(m_vfxPreviews);
    releaseDependent(m_terrainPreviews);
}

bool AssetBrowserPanel::DrawOriginalTypeIcon(ImVec2 origin, float size, const Entry& entry)
{
    const auto type = assetthumbnail::TypeIcon(entry.ext);
    if (type == assetthumbnail::Icon::NONE || !m_resources || !m_thumbnailGui) return false;
    constexpr std::array<uint32_t, 6> SIZES{ 16, 24, 32, 64, 128, 256 };
    const float pixels = size * std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    size_t scaleIndex = 0;
    while (scaleIndex + 1 < SIZES.size() && SIZES[scaleIndex] < pixels) ++scaleIndex;
    const size_t index = static_cast<size_t>(type) * SIZES.size() + scaleIndex;
    auto& handle = m_typeIcons[index];
    if (!m_typeIconAttempted[index]) {
        m_typeIconAttempted[index] = true;
        const std::string name = std::string(assetthumbnail::IconName(type)) + std::to_string(SIZES[scaleIndex]) + ".png";
        const auto root = util::FileSystem::PathFromUtf8(m_typeIconRoot);
        auto source = util::FileSystem::PathFromUtf8(__FILE__);
        for (int parent = 0; parent < 6; ++parent) source = source.parent_path();
        const std::array<std::filesystem::path, 3> roots{
            root / "share/fbzz/Assets/Icons/AssetBrowser", root / "Assets/Icons/AssetBrowser",
            source / "Assets/Icons/AssetBrowser"
        };
        for (const auto& candidate : roots) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(candidate / name, error)) continue;
            const auto image = assetthumbnail::Decode(candidate / name);
            if (image.state == assetthumbnail::State::READY)
                handle = m_resources->CreateTexture(image.rgba.data(), image.width, image.height);
            if (handle.IsValid()) break;
        }
    }
    if (!handle.IsValid()) return false;
    void* raw = m_thumbnailGui->GetImTextureID(handle, *m_resources);
    if (!raw) return false;
    const auto id = static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(raw));
    ImGui::GetWindowDrawList()->AddImage(id, origin, { origin.x + size, origin.y + size });
    return true;
}

void AssetBrowserPanel::DrawAssetPreviewState(ImVec2 origin, float size, const Entry& entry,
                                             assetthumbnail::State state)
{
    if (state == assetthumbnail::State::READY) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float radius = std::clamp(size * 0.13f, 3.0f, 9.0f);
    const ImVec2 center{ origin.x + size - radius - 1, origin.y + size - radius - 1 };
    draw->AddCircleFilled(center, radius + 1, IM_COL32(20, 23, 28, 240));
    const ImU32 color = state == assetthumbnail::State::FAILED ? IM_COL32(255, 174, 154, 255)
        : state == assetthumbnail::State::PENDING ? IM_COL32(182, 223, 235, 255) : IM_COL32(200, 205, 215, 255);
    draw->AddCircle(center, radius, color, 16, 1.5f);
    if (state == assetthumbnail::State::PENDING) {
        draw->AddLine(center, { center.x, center.y - radius * 0.55f }, color, 1.5f);
        draw->AddLine(center, { center.x + radius * 0.5f, center.y }, color, 1.5f);
    } else if (state == assetthumbnail::State::FAILED) {
        draw->AddLine({ center.x, center.y - radius * 0.5f }, { center.x, center.y + radius * 0.1f }, color, 1.5f);
        draw->AddCircleFilled({ center.x, center.y + radius * 0.5f }, 1, color);
    } else {
        draw->AddLine({ center.x - radius * 0.45f, center.y + radius * 0.45f },
                      { center.x + radius * 0.45f, center.y - radius * 0.45f }, color, 1.5f);
    }
    if (ImGui::IsMouseHoveringRect(origin, { origin.x + size, origin.y + size })) {
        const char* message = state == assetthumbnail::State::PENDING ? "Preview loading"
            : state == assetthumbnail::State::FAILED ? "Preview failed; changes or Refresh allow retry"
            : "Preview unavailable for this content or size; asset-type icon shown";
        ImGui::SetTooltip("%s\n%s", entry.name.c_str(), message);
    }
}

void AssetBrowserPanel::OnShutdown()
{
    ClearAllAssetPreviews();
    if (m_resources) for (auto handle : m_typeIcons) if (handle.IsValid()) m_resources->Release(handle);
    m_typeIcons = {};
    m_typeIconAttempted = {};
    m_thumbnailGui = nullptr;
    m_resources = nullptr;
}

} /// @note namespace fbzz::editor
