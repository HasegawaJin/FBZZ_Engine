/// @file    TexturePreview.cpp
/// @brief   Texture アセットを Inspector に描画する。
/// @author  Hasegawa Jin
/// @date    2026-09-24
#include <Editor/Panels/TexturePreview.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace fbzz::editor {
namespace {

constexpr double kFileCheckInterval = 0.5;
constexpr float kCheckerCell = 12.0f;

void DrawTextureChecker(ImDrawList* drawList, ImVec2 origin, float side)
{
    drawList->AddRectFilled(origin, { origin.x + side, origin.y + side },
                            IM_COL32(58, 60, 66, 255), 4.0f);
    drawList->PushClipRect(origin, { origin.x + side, origin.y + side }, true);
    const int cells = static_cast<int>(std::ceil(side / kCheckerCell));
    for (int y = 0; y < cells; ++y) {
        for (int x = y & 1; x < cells; x += 2) {
            const ImVec2 cellMin{ origin.x + x * kCheckerCell,
                                  origin.y + y * kCheckerCell };
            drawList->AddRectFilled(cellMin,
                { (std::min)(cellMin.x + kCheckerCell, origin.x + side),
                  (std::min)(cellMin.y + kCheckerCell, origin.y + side) },
                IM_COL32(40, 42, 47, 255));
        }
    }
    drawList->PopClipRect();
}

} /// @note namespace

void TexturePreviewView::Draw(EditorContext& ctx,
                              std::string_view sourcePath,
                              float previewHeight)
{
    const std::uint64_t resetVersion = ctx.resources ? ctx.resources->GetResetVersion() : 0;
    if (sourcePath != m_sourcePath || ctx.resources != m_resourceOwner ||
        resetVersion != m_resetVersion) {
        m_sourcePath = sourcePath;
        m_resourceOwner = ctx.resources;
        m_texture = {};
        m_writeTime = 0;
        m_resetVersion = resetVersion;
        m_nextFileCheck = 0.0;
        m_fileExists = false;
        m_checkedFile = false;
    }

    if (!ctx.resources || !ctx.imguiRenderer) {
        ImGui::TextDisabled("Texture preview is unavailable.");
        return;
    }

    const double now = ImGui::GetTime();
    if (now >= m_nextFileCheck) {
        std::error_code error;
        const auto writeTime = std::filesystem::last_write_time(
            util::FileSystem::PathFromUtf8(m_sourcePath), error);
        const bool exists = !error;
        const std::int64_t revision = exists
            ? static_cast<std::int64_t>(writeTime.time_since_epoch().count()) : 0;
        const bool firstCheck = !m_checkedFile;
        const bool changed = !firstCheck &&
            (exists != m_fileExists || (exists && revision != m_writeTime));
        m_fileExists = exists;
        m_writeTime = revision;
        m_checkedFile = true;
        m_nextFileCheck = now + kFileCheckInterval;

        if (!exists) {
            m_texture = {};
        } else if (changed) {
            /// @note 同じパスを参照する材質とサムネイルのハンドルも最新画像へ差し替える。
            m_texture = ctx.resources->ReloadTexture(m_sourcePath);
        } else if (firstCheck || (m_texture.IsValid() && !ctx.resources->Get(m_texture))) {
            m_texture = ctx.resources->LoadTexture(m_sourcePath);
        }
    }

    if (!m_fileExists) {
        ImGui::TextDisabled("Source image not found.");
        return;
    }
    const renderer::ITexture* texture = ctx.resources->Get(m_texture);
    if (!texture) {
        ImGui::TextDisabled("Could not load this texture.");
        return;
    }
    void* rawId = ctx.imguiRenderer->GetImTextureID(m_texture, *ctx.resources);
    if (!rawId) {
        ImGui::TextDisabled("Texture GPU view is unavailable.");
        return;
    }

    const float width = static_cast<float>((std::max)(1u, texture->GetWidth()));
    const float height = static_cast<float>((std::max)(1u, texture->GetHeight()));
    ImGui::TextDisabled("%u x %u", texture->GetWidth(), texture->GetHeight());

    const float side = (std::max)(1.0f,
        (std::min)(ImGui::GetContentRegionAvail().x, (std::max)(previewHeight, 96.0f)));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(this);
    ImGui::InvisibleButton("##TextureImage", { side, side });
    ImGui::PopID();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    DrawTextureChecker(drawList, origin, side);
    const float scale = (std::min)(side / width, side / height);
    const ImVec2 imageSize{ width * scale, height * scale };
    const ImVec2 imageMin{ origin.x + (side - imageSize.x) * 0.5f,
                           origin.y + (side - imageSize.y) * 0.5f };
    drawList->AddImage(widgets::ToImTextureID(rawId), imageMin,
                       { imageMin.x + imageSize.x, imageMin.y + imageSize.y });
    drawList->AddRect(origin, { origin.x + side, origin.y + side },
                      IM_COL32(95, 100, 112, 230), 4.0f, 0, 1.0f);
}

} /// @note namespace fbzz::editor
