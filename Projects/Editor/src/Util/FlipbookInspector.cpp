/// @file    FlipbookInspector.cpp
/// @brief   .mat の [particle] フリップブック節 — アトラスのプレビュー・コマ範囲の編集・アトラス生成
/// @author  Hasegawa Jin
/// @date    2026-09-11

#include <Editor/Util/FlipbookInspector.hpp>

#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FlipbookAtlasBaker.hpp>
#include <Engine/Asset/FlipbookMotionVectors.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/ParticleMaterialSettings.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <Engine/Asset/TextureAnalysis.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

using asset::ParticleFlipbookSettings;
using scene::ParticleFlipbookMode;

constexpr ImVec4 kWarningColor{ 1.0f, 0.72f, 0.30f, 1.0f };
constexpr ImVec4 kErrorColor{ 1.0f, 0.40f, 0.30f, 1.0f };
constexpr ImU32  kGridLineColor   = IM_COL32(255, 255, 255, 70);
constexpr ImU32  kInactiveCell    = IM_COL32(0, 0, 0, 150);
constexpr ImU32  kHoverCell       = IM_COL32(255, 255, 255, 40);
constexpr ImU32  kRangeFirstColor = IM_COL32(90, 220, 120, 255);
constexpr ImU32  kRangeLastColor  = IM_COL32(240, 150, 60, 255);
constexpr ImU32  kCurrentColor    = IM_COL32(255, 230, 80, 255);

/// @name テクスチャ参照

/// Project ごとの Generated 配下へ出し、Engine 同梱 Assets を誤って変更しない。
std::string ProceduralVFXOutputDirectory(const std::string& projectRoot)
{
    if (projectRoot.empty()) return "Assets/Textures/Generated/VFX";
    return projectRoot + "/Assets/Textures/Generated/VFX";
}

/// テクスチャ参照を画像ローダーが開ける実パスへ変換する。
/// @note [textures] は guid: 参照でも保存されるため、projectRoot を継ぎ足すだけでは開けない。
std::string ResolveTextureDiskPath(const std::string& projectRoot, const std::string& reference)
{
    if (reference.empty()) return {};
    if (std::string resolved = asset::AssetManager::ResolveAssetPath(reference); !resolved.empty())
        return resolved;
    return ToProjectAssetDiskPath(projectRoot, reference);
}

const std::string& TextureSlot(const asset::MaterialAsset& material, const char* slot)
{
    static const std::string kEmpty;
    const auto it = material.textures.find(slot);
    return it != material.textures.end() ? it->second : kEmpty;
}

std::string FileNameOf(const std::string& path)
{
    return util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).filename());
}

struct AtlasPreview {
    std::string   reference;
    std::uint64_t resetVersion = 0;
    void*         textureId    = nullptr;
    int           width        = 0;
    int           height       = 0;
};

/// Inspector が同時に開く .mat は 1 つなので、直近の 1 枚だけ覚えれば足りる。
/// @note パス解決とハンドル引きを毎フレームやり直すと Inspector を開いているだけでファイル探索が走り続けるため、読めなかったことも覚え失敗の再試行を繰り返さない。
AtlasPreview s_atlasPreview;

/// 生成ツールは同じパスへ上書きすることがある。寸法が変わるので覚えた値を捨てる。
void InvalidateAtlasPreview()
{
    s_atlasPreview = {};
}

const AtlasPreview& ResolveAtlasPreview(const std::string& reference,
                                        renderer::ResourceManager* resources,
                                        renderer::IImGuiRenderer* imguiRenderer)
{
    static const AtlasPreview kNone;
    if (reference.empty() || resources == nullptr || imguiRenderer == nullptr) return kNone;
    /// @note デバイスリセット後は過去のテクスチャ ID が全て無効になる。
    const std::uint64_t version = resources->GetResetVersion();
    if (s_atlasPreview.reference == reference && s_atlasPreview.resetVersion == version)
        return s_atlasPreview;

    s_atlasPreview              = {};
    s_atlasPreview.reference    = reference;
    s_atlasPreview.resetVersion = version;
    const auto handle = resources->LoadTexture(asset::AssetManager::ResolveAssetPath(reference));
    if (!handle.IsValid()) return s_atlasPreview;
    s_atlasPreview.textureId = imguiRenderer->GetImTextureID(handle, *resources);
    if (const renderer::ITexture* texture = resources->Get(handle)) {
        s_atlasPreview.width  = static_cast<int>(texture->GetWidth());
        s_atlasPreview.height = static_cast<int>(texture->GetHeight());
    }
    return s_atlasPreview;
}

/// @name 描画の小道具

struct FrameUv {
    ImVec2 min;
    ImVec2 max;
};

/// 左上から行優先。ParticlePass の SpriteRectForFrame と同じ並び。
FrameUv FrameUvRect(int frame, int columns, int rows)
{
    const int x = frame % columns;
    const int y = frame / columns;
    const float invColumns = 1.0f / static_cast<float>(columns);
    const float invRows    = 1.0f / static_cast<float>(rows);
    return { { static_cast<float>(x) * invColumns, static_cast<float>(y) * invRows },
             { static_cast<float>(x + 1) * invColumns, static_cast<float>(y + 1) * invRows } };
}

void DrawChecker(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, float cell)
{
    const int countX = static_cast<int>(std::ceil((max.x - min.x) / cell));
    const int countY = static_cast<int>(std::ceil((max.y - min.y) / cell));
    for (int y = 0; y < countY; ++y) {
        for (int x = 0; x < countX; ++x) {
            const ImVec2 a{ min.x + cell * static_cast<float>(x), min.y + cell * static_cast<float>(y) };
            const ImVec2 b{ (std::min)(a.x + cell, max.x), (std::min)(a.y + cell, max.y) };
            drawList->AddRectFilled(a, b, ((x + y) & 1) == 0
                ? IM_COL32(70, 70, 70, 255) : IM_COL32(42, 42, 42, 255));
        }
    }
}

void DrawStatus(const std::string& status, bool isError)
{
    if (status.empty()) return;
    if (isError) ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::TextWrapped("%s", status.c_str());
    if (isError) ImGui::PopStyleColor();
}

void DisabledWrapped(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/// 警告 1 行と、あれば直すボタン。
/// @return 直すボタンが押されたら true
bool WarningRow(const char* text, const char* fixLabel = nullptr)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
    ImGui::TextWrapped("! %s", text);
    ImGui::PopStyleColor();
    if (fixLabel == nullptr) return false;
    ImGui::Indent();
    const bool fixed = ImGui::SmallButton(fixLabel);
    ImGui::Unindent();
    return fixed;
}

/// Random Row では行の全列が再生対象になり、Start / End は使われない。
int PlayedSpan(const ParticleFlipbookSettings& flipbook, const asset::FlipbookFrameRange& range)
{
    if (flipbook.spriteRandomRow && range.rows > 1) return range.columns - 1;
    return range.last - range.first;
}

bool IsTimedMode(ParticleFlipbookMode mode)
{
    return mode == ParticleFlipbookMode::FramesPerSecond || mode == ParticleFlipbookMode::PingPong;
}

/// @name UI 状態 (Inspector は 1 つしか開かないため、ファイル内 static で足りる)

struct PreviewState {
    bool  playing  = true;
    float time     = 0.0f;
    float lifetime = 1.0f;
    bool  lockSeed = false;
    float seed     = 0.37f;
};

struct GridDetectState {
    std::vector<asset::FlipbookGridCandidate> candidates;
    std::string status;
    bool        statusIsError = false;
};

struct SequenceBakeState {
    std::string              firstFrame;
    std::string              scannedFor;
    std::vector<std::string> frames;
    std::string              scanError;
    int                      columns   = 0;
    bool                     overwrite = false;
    std::string              status;
    bool                     statusIsError = false;
};

struct ProceduralState {
    int         preset       = 0;
    int         frameSize    = 128;
    int         columns      = 8;
    int         rows         = 2;
    int         seed         = 1;
    float       noiseScale   = 4.0f;
    float       warpStrength = 0.65f;
    bool        generateMotionVectors = false;
    std::string status;
    bool        statusIsError = false;
};

PreviewState      s_preview;
GridDetectState   s_gridDetect;
SequenceBakeState s_sequenceBake;
ProceduralState   s_procedural;
std::string       s_motionVectorStatus;
bool              s_motionVectorStatusIsError = false;

/// @name 概要

void DrawSummary(const ParticleFlipbookSettings& flipbook, const asset::FlipbookFrameRange& range,
                 const AtlasPreview& atlas)
{
    const int count  = range.columns * range.rows;
    const int played = PlayedSpan(flipbook, range) + 1;
    const float fps  = (std::max)(flipbook.flipbookFramesPerSecond, 0.0f);

    std::string text;
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%d x %d = %d frames", range.columns, range.rows, count);
    text = buffer;
    if (flipbook.spriteRandomRow && range.rows > 1)
        std::snprintf(buffer, sizeof(buffer), "  |  %d frames per row, row picked per particle", range.columns);
    else
        std::snprintf(buffer, sizeof(buffer), "  |  plays %d-%d (%d)", range.first, range.last, played);
    text += buffer;
    if (atlas.width > 0 && atlas.height > 0) {
        std::snprintf(buffer, sizeof(buffer), "  |  %d x %d px/frame",
                      atlas.width / range.columns, atlas.height / range.rows);
        text += buffer;
    }
    if (flipbook.flipbookMode == ParticleFlipbookMode::FramesPerSecond && fps > 0.0f) {
        std::snprintf(buffer, sizeof(buffer), "  |  loop %.2fs", static_cast<float>(played) / fps);
        text += buffer;
    } else if (flipbook.flipbookMode == ParticleFlipbookMode::PingPong && fps > 0.0f) {
        std::snprintf(buffer, sizeof(buffer), "  |  loop %.2fs",
                      static_cast<float>((std::max)((played - 1) * 2, 1)) / fps);
        text += buffer;
    }
    DisabledWrapped(text.c_str());
}

/// @name アトラス

ImVec2 FitAtlasSize(const AtlasPreview& atlas, int columns, int rows)
{
    const float maxSide = (std::min)(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 22.0f);
    const float aspect = atlas.width > 0 && atlas.height > 0
        ? static_cast<float>(atlas.height) / static_cast<float>(atlas.width)
        : static_cast<float>(rows) / static_cast<float>(columns);
    float width  = maxSide;
    float height = maxSide * aspect;
    if (height > maxSide) {
        height = maxSide;
        width  = maxSide / aspect;
    }
    return { (std::max)(width, 16.0f), (std::max)(height, 16.0f) };
}

/// クリックで Start、右クリック / Shift+クリックで End を指定する。
/// @return 範囲を変えたら true
bool DrawAtlasGrid(ParticleFlipbookSettings& flipbook, const AtlasPreview& atlas,
                   const asset::FlipbookFrameSample& live)
{
    bool changed = false;
    const asset::FlipbookFrameRange range = asset::ResolveFlipbookRange(flipbook);
    const int columns    = range.columns;
    const int rows       = range.rows;
    const int frameCount = columns * rows;
    const bool rowDriven = flipbook.spriteRandomRow && rows > 1;

    const ImVec2 size   = FitAtlasSize(atlas, columns, rows);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end{ origin.x + size.x, origin.y + size.y };
    ImGui::InvisibleButton("##atlas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    DrawChecker(drawList, origin, end, 10.0f);
    if (atlas.textureId != nullptr)
        drawList->AddImage(widgets::ToImTextureID(atlas.textureId), origin, end);

    const float cellWidth  = size.x / static_cast<float>(columns);
    const float cellHeight = size.y / static_cast<float>(rows);
    const auto cellMin = [&](int frame) {
        return ImVec2{ origin.x + static_cast<float>(frame % columns) * cellWidth,
                       origin.y + static_cast<float>(frame / columns) * cellHeight };
    };
    const auto cellMax = [&](int frame) {
        const ImVec2 a = cellMin(frame);
        return ImVec2{ a.x + cellWidth, a.y + cellHeight };
    };

    /// @note 再生されないコマを沈める。何が再生されるかを «範囲の数字» ではなく絵で読ませる。
    if (!rowDriven) {
        for (int frame = 0; frame < frameCount; ++frame) {
            if (frame >= range.first && frame <= range.last) continue;
            drawList->AddRectFilled(cellMin(frame), cellMax(frame), kInactiveCell);
        }
    }
    for (int column = 1; column < columns; ++column) {
        const float x = origin.x + cellWidth * static_cast<float>(column);
        drawList->AddLine({ x, origin.y }, { x, end.y }, kGridLineColor);
    }
    for (int row = 1; row < rows; ++row) {
        const float y = origin.y + cellHeight * static_cast<float>(row);
        drawList->AddLine({ origin.x, y }, { end.x, y }, kGridLineColor);
    }
    drawList->AddRect(origin, end, IM_COL32(0, 0, 0, 160));

    if (cellWidth >= ImGui::GetFontSize() * 1.6f && cellHeight >= ImGui::GetFontSize()) {
        char label[8];
        for (int frame = 0; frame < frameCount; ++frame) {
            std::snprintf(label, sizeof(label), "%d", frame);
            const ImVec2 a = cellMin(frame);
            drawList->AddText({ a.x + 3.0f, a.y + 1.0f }, IM_COL32(255, 255, 255, 170), label);
        }
    }

    if (!rowDriven && frameCount > 1) {
        drawList->AddRect(cellMin(range.first), cellMax(range.first), kRangeFirstColor, 0.0f, 0, 2.0f);
        drawList->AddRect(cellMin(range.last), cellMax(range.last), kRangeLastColor, 0.0f, 0, 2.0f);
    }
    if (live.blend > 0.0f && live.nextFrame != live.frame) {
        const ImU32 fade = IM_COL32(255, 230, 80, static_cast<int>(255.0f * live.blend));
        drawList->AddRect(cellMin(live.nextFrame), cellMax(live.nextFrame), fade, 0.0f, 0, 1.5f);
    }
    drawList->AddRect(cellMin(live.frame), cellMax(live.frame), kCurrentColor, 0.0f, 0, 2.5f);

    if (!hovered) return false;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const int column = std::clamp(static_cast<int>((mouse.x - origin.x) / cellWidth), 0, columns - 1);
    const int row    = std::clamp(static_cast<int>((mouse.y - origin.y) / cellHeight), 0, rows - 1);
    const int frame  = row * columns + column;
    drawList->AddRectFilled(cellMin(frame), cellMax(frame), kHoverCell);

    ImGui::BeginTooltip();
    if (atlas.textureId != nullptr) {
        const FrameUv uv = FrameUvRect(frame, columns, rows);
        const float zoom = ImGui::GetFontSize() * 7.0f;
        const float cellAspect = cellHeight / cellWidth;
        ImGui::Image(widgets::ToImTextureID(atlas.textureId),
                     { zoom, zoom * cellAspect }, uv.min, uv.max);
    }
    ImGui::Text("Frame %d  (column %d, row %d)", frame, column, row);
    if (!rowDriven) ImGui::TextDisabled("Click: Start  /  Right-Click or Shift+Click: End");
    ImGui::EndTooltip();

    if (rowDriven) return false;
    const bool leftClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool setEnd = ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        || (leftClicked && ImGui::GetIO().KeyShift);
    if (setEnd) {
        if (frame < range.first) {
            flipbook.spriteStartFrame = frame;
            flipbook.spriteEndFrame   = range.first;
        } else {
            /// @note 最終コマは «最後まで» (0) で保存する。後から行を足してもそのまま最後まで流れる。
            flipbook.spriteEndFrame = frame == frameCount - 1 ? 0 : (std::max)(frame, 1);
        }
        changed = true;
    } else if (leftClicked) {
        flipbook.spriteStartFrame = frame;
        if (flipbook.spriteEndFrame > 0 && flipbook.spriteEndFrame < frame)
            flipbook.spriteEndFrame = (std::max)(frame, 1);
        changed = true;
    }
    return changed;
}

/// @name ライブプレビュー

/// 1 粒子が寿命ごとに生まれ直すとみなして、いまのコマを引く。
asset::FlipbookFrameSample SamplePreview(const ParticleFlipbookSettings& flipbook, const PreviewState& state)
{
    const float lifetime   = (std::max)(state.lifetime, 0.05f);
    const float cycleIndex = std::floor(state.time / lifetime);
    const float age        = state.time - cycleIndex * lifetime;
    float seed = state.seed;
    if (!state.lockSeed) {
        /// @note Random Row / Random Start の «ばらけ方» は 1 粒子を見続けても分からない。
        ///       周回ごとに別の粒子として乱数を引き直す (黄金比で散らすと偏りが出にくい)。
        const float shifted = state.seed + cycleIndex * 0.6180339f;
        seed = shifted - std::floor(shifted);
    }
    return asset::EvaluateFlipbookFrame(flipbook, age / lifetime, age, seed);
}

void DrawFramePreview(const AtlasPreview& atlas, const asset::FlipbookFrameRange& range,
                      const asset::FlipbookFrameSample& sample, float side)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({ side, side });
    const ImVec2 end{ origin.x + side, origin.y + side };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    DrawChecker(drawList, origin, end, side / 8.0f);

    if (atlas.textureId != nullptr) {
        /// @note コマの縦横比を保って枠へ収める。正方形へ引き伸ばすと細長いコマの動きが読めない。
        const float cellWidth  = atlas.width > 0 ? static_cast<float>(atlas.width) / static_cast<float>(range.columns) : 1.0f;
        const float cellHeight = atlas.height > 0 ? static_cast<float>(atlas.height) / static_cast<float>(range.rows) : 1.0f;
        const float scale = side / (std::max)(cellWidth, cellHeight);
        const ImVec2 size{ cellWidth * scale, cellHeight * scale };
        const ImVec2 a{ origin.x + (side - size.x) * 0.5f, origin.y + (side - size.y) * 0.5f };
        const ImVec2 b{ a.x + size.x, a.y + size.y };
        const ImTextureID id = widgets::ToImTextureID(atlas.textureId);
        /// @note Frame Blending は 2 コマを不透明度で重ねた近似で見せる (シェーダーは線形補間)。
        const FrameUv current = FrameUvRect(sample.frame, range.columns, range.rows);
        const int currentAlpha = static_cast<int>(255.0f * (1.0f - sample.blend) + 0.5f);
        drawList->AddImage(id, a, b, current.min, current.max, IM_COL32(255, 255, 255, currentAlpha));
        if (sample.blend > 0.0f) {
            const FrameUv next = FrameUvRect(sample.nextFrame, range.columns, range.rows);
            const int nextAlpha = static_cast<int>(255.0f * sample.blend + 0.5f);
            drawList->AddImage(id, a, b, next.min, next.max, IM_COL32(255, 255, 255, nextAlpha));
        }
    } else {
        char label[16];
        std::snprintf(label, sizeof(label), "#%d", sample.frame);
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        drawList->AddText({ origin.x + (side - textSize.x) * 0.5f, origin.y + (side - textSize.y) * 0.5f },
                          IM_COL32_WHITE, label);
    }
    drawList->AddRect(origin, end, ImGui::GetColorU32(ImGuiCol_Border));
}

void DrawPreviewControls(const asset::FlipbookFrameSample& live)
{
    ImGui::BeginGroup();
    if (ImGui::SmallButton(s_preview.playing ? "Pause" : "Play"))
        s_preview.playing = !s_preview.playing;
    ImGui::SameLine();
    if (ImGui::SmallButton("Restart")) s_preview.time = 0.0f;

    const float width    = ImGui::GetFontSize() * 9.0f;
    const float lifetime = (std::max)(s_preview.lifetime, 0.05f);
    const float cycleStart = std::floor(s_preview.time / lifetime) * lifetime;
    float age = s_preview.time - cycleStart;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("##scrub", &age, 0.0f, lifetime, "Age %.2fs")) {
        s_preview.playing = false;
        s_preview.time    = cycleStart + age;
    }
    ImGui::SetNextItemWidth(width);
    ImGui::DragFloat("Lifetime##preview", &s_preview.lifetime, 0.01f, 0.05f, 30.0f, "%.2fs");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("プレビュー専用の寿命です。実際の寿命は ParticleEmitter の Lifetime が決めます。\n"
                          "Lifetime モードでは、この長さで再生範囲を 1 回流します。");
    ImGui::Checkbox("Lock Seed", &s_preview.lockSeed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("外すと周回ごとに別の粒子として乱数を引き直します。\n"
                          "Random Row / Random Start / Random のばらけ方を確認するときは外したままにします。");
    if (s_preview.lockSeed) {
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Seed##preview", &s_preview.seed, 0.0f, 1.0f, "%.2f");
    }
    if (live.blend > 0.0f)
        ImGui::Text("Frame %d > %d  (%.0f%%)", live.frame, live.nextFrame, live.blend * 100.0f);
    else
        ImGui::Text("Frame %d", live.frame);
    ImGui::EndGroup();
}

/// @name 検査

/// エラーにならないまま «なんとなく変な絵» になる設定を並べる。直せるものはボタンを出す。
bool DrawFlipbookIssues(ParticleFlipbookSettings& flipbook, const asset::MaterialAsset& material,
                        const AtlasPreview& atlas)
{
    bool changed = false;
    const int frameCount = flipbook.FrameCount();
    const int columns    = (std::max)(flipbook.spriteColumns, 1);
    const int rows       = (std::max)(flipbook.spriteRows, 1);
    char text[256];

    ImGui::PushID("issues");
    if (flipbook.spriteStartFrame >= frameCount) {
        std::snprintf(text, sizeof(text),
                      "Start Frame %d がコマ数 %d を超えています (最終コマに丸めて再生されます)",
                      flipbook.spriteStartFrame, frameCount);
        if (WarningRow(text, "Reset Start##start")) { flipbook.spriteStartFrame = 0; changed = true; }
    }
    if (flipbook.spriteEndFrame >= frameCount) {
        std::snprintf(text, sizeof(text),
                      "End Frame %d がコマ数 %d を超えています (最終コマに丸めて再生されます)",
                      flipbook.spriteEndFrame, frameCount);
        if (WarningRow(text, "Play To Last##end")) { flipbook.spriteEndFrame = 0; changed = true; }
    }
    if (flipbook.spriteEndFrame > 0 && flipbook.spriteEndFrame < flipbook.spriteStartFrame
        && flipbook.spriteStartFrame < frameCount) {
        if (WarningRow("End Frame が Start Frame より前です (Start の 1 コマで止まります)", "Swap##range")) {
            std::swap(flipbook.spriteStartFrame, flipbook.spriteEndFrame);
            flipbook.spriteEndFrame = (std::max)(flipbook.spriteEndFrame, 1);
            changed = true;
        }
    }
    if (flipbook.spriteRandomRow && rows <= 1) {
        if (WarningRow("Random Row は行が 1 本だと効きません", "Turn Off##randomRow")) {
            flipbook.spriteRandomRow = false;
            changed = true;
        }
    }
    if (IsTimedMode(flipbook.flipbookMode) && flipbook.flipbookFramesPerSecond <= 0.0f) {
        if (WarningRow("FPS が 0 のため最初のコマで止まります", "Set 24 fps##fps")) {
            flipbook.flipbookFramesPerSecond = 24.0f;
            changed = true;
        }
    }
    if (flipbook.flipbookFrameBlending && flipbook.flipbookMode == ParticleFlipbookMode::RandomFrame) {
        if (WarningRow("Random ではコマが固定なので Frame Blending は効きません (CPU 縮退だけが残ります)",
                       "Turn Off##blend")) {
            flipbook.flipbookFrameBlending = false;
            changed = true;
        }
    }
    if (frameCount <= 1
        && (flipbook.flipbookFrameBlending || flipbook.spriteRandomStartFrame || flipbook.motionVectorFlipbook))
        WarningRow("1 枚絵 (1 x 1) なので、フリップブックの再生設定は効きません");
    if (atlas.width > 0 && atlas.height > 0
        && (atlas.width % columns != 0 || atlas.height % rows != 0)) {
        std::snprintf(text, sizeof(text),
                      "アトラス %d x %d px は %d x %d で割り切れません。コマの端に隣のコマがにじみます",
                      atlas.width, atlas.height, columns, rows);
        WarningRow(text);
    }
    if (flipbook.motionVectorFlipbook && TextureSlot(material, "tex5").empty())
        WarningRow("Motion Vector アトラス (tex5) が未設定のため、Motion Vector Blending は効きません");
    ImGui::PopID();
    return changed;
}

/// @name 連番画像からアトラスを焼く

/// "Explosion_012" -> prefix "Explosion_"。末尾に数字が無ければ連番ではない。
bool SplitSequenceStem(const std::string& stem, std::string& prefix, std::string& digits)
{
    std::size_t cut = stem.size();
    while (cut > 0 && std::isdigit(static_cast<unsigned char>(stem[cut - 1]))) --cut;
    if (cut == stem.size()) return false;
    prefix = stem.substr(0, cut);
    digits = stem.substr(cut);
    return true;
}

std::string ToLowerAscii(std::string value)
{
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

/// 1 枚目と同じフォルダー・同じ接頭辞・同じ拡張子の画像を番号順に集める。
/// @note 番号で並べる: 名前順だと 0 詰めしていない連番 ("fx_9" と "fx_10") が逆転する。
std::vector<std::string> CollectSequenceFrames(const std::string& firstFrameDiskPath, std::string& error)
{
    namespace fs = std::filesystem;
    std::vector<std::string> frames;
    const fs::path first = util::FileSystem::PathFromUtf8(firstFrameDiskPath);
    std::string prefix;
    std::string digits;
    if (!SplitSequenceStem(util::FileSystem::PathToUtf8(first.stem()), prefix, digits)) {
        error = "ファイル名の末尾が番号ではありません (例: Explosion_000.png)";
        return frames;
    }
    const std::string extension = ToLowerAscii(util::FileSystem::PathToUtf8(first.extension()));

    std::vector<std::pair<long long, std::string>> numbered;
    std::error_code ec;
    for (fs::directory_iterator it(first.parent_path(), ec), last; !ec && it != last; it.increment(ec)) {
        std::error_code typeError;
        if (!it->is_regular_file(typeError)) continue;
        const fs::path& path = it->path();
        if (ToLowerAscii(util::FileSystem::PathToUtf8(path.extension())) != extension) continue;
        std::string candidatePrefix;
        std::string candidateDigits;
        if (!SplitSequenceStem(util::FileSystem::PathToUtf8(path.stem()), candidatePrefix, candidateDigits))
            continue;
        /// @note 桁が多すぎる番号は日付などで、連番ではない。
        if (candidatePrefix != prefix || candidateDigits.size() > 9) continue;
        long long number = 0;
        for (const char c : candidateDigits) number = number * 10 + (c - '0');
        numbered.emplace_back(number, util::FileSystem::PathToUtf8(path));
    }
    if (ec) {
        error = "フォルダーを読めませんでした: " + ec.message();
        return frames;
    }
    std::sort(numbered.begin(), numbered.end());
    frames.reserve(numbered.size());
    for (auto& [number, path] : numbered) frames.push_back(std::move(path));
    if (frames.size() < 2) error = "連番が 1 枚しか見つかりません";
    return frames;
}

std::string SequenceAtlasOutputPath(const std::string& firstFrameDiskPath)
{
    if (firstFrameDiskPath.empty()) return {};
    const std::filesystem::path first = util::FileSystem::PathFromUtf8(firstFrameDiskPath);
    std::string prefix;
    std::string digits;
    if (!SplitSequenceStem(util::FileSystem::PathToUtf8(first.stem()), prefix, digits)) return {};
    while (!prefix.empty() && (prefix.back() == '_' || prefix.back() == '-' || prefix.back() == ' ' || prefix.back() == '.'))
        prefix.pop_back();
    if (prefix.empty()) prefix = "Sequence";
    return util::FileSystem::PathToUtf8(first.parent_path() / util::FileSystem::PathFromUtf8(prefix + "_Atlas.png"));
}

bool DrawSequenceBaker(asset::MaterialAsset& material, const std::string& projectRoot)
{
    bool changed = false;
    SequenceBakeState& state = s_sequenceBake;
    ParticleFlipbookSettings& flipbook = material.particle.flipbook;

    widgets::AssetPathField("First Frame", state.firstFrame, widgets::kTextureAssetFilter, projectRoot);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("連番の 1 枚目 (例: Explosion_000.png) を指定します。\n"
                          "同じフォルダーの同じ接頭辞・同じ拡張子の画像を番号順に集めます。");
    if (state.scannedFor != state.firstFrame) {
        state.scannedFor = state.firstFrame;
        state.frames.clear();
        state.scanError.clear();
        if (!state.firstFrame.empty())
            state.frames = CollectSequenceFrames(ResolveTextureDiskPath(projectRoot, state.firstFrame),
                                                 state.scanError);
    }
    if (!state.scanError.empty()) {
        ImGui::TextColored(kWarningColor, "%s", state.scanError.c_str());
    } else if (!state.frames.empty()) {
        ImGui::Text("%d frames:  %s  ...  %s", static_cast<int>(state.frames.size()),
                    FileNameOf(state.frames.front()).c_str(), FileNameOf(state.frames.back()).c_str());
    }

    ImGui::DragInt("Columns##bake", &state.columns, 0.1f, 0, 64, state.columns == 0 ? "Auto" : "%d");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 (Auto) なら正方形に近い並びを選びます。行数はコマ数から決まります。");
    ImGui::Checkbox("Overwrite##bake", &state.overwrite);

    const std::string outputPath = state.frames.empty() ? std::string{} : SequenceAtlasOutputPath(state.frames.front());
    if (!outputPath.empty()) ImGui::TextDisabled("Output: %s", FileNameOf(outputPath).c_str());

    ImGui::BeginDisabled(state.frames.size() < 2 || outputPath.empty());
    if (ImGui::Button("Bake & Assign##sequence", { -1.0f, 0.0f })) {
        asset::FlipbookAtlasBakeSettings settings;
        settings.framePaths = state.frames;
        settings.outputPath = outputPath;
        settings.columns    = state.columns;
        settings.overwrite  = state.overwrite;
        const asset::FlipbookAtlasBakeResult result = asset::BakeFlipbookAtlas(settings);
        state.status        = result.message;
        state.statusIsError = !result.success;
        if (result.success) {
            material.textures["albedo"] = NormalizeAssetPath(result.outputPath);
            flipbook.spriteColumns    = result.columns;
            flipbook.spriteRows       = result.rows;
            flipbook.spriteStartFrame = 0;
            flipbook.spriteEndFrame   = 0;
            flipbook.spriteRandomRow  = false;
            InvalidateAtlasPreview();
            changed = true;
        }
    }
    ImGui::EndDisabled();
    DrawStatus(state.status, state.statusIsError);
    return changed;
}

/// @name 手続き生成

bool DrawProceduralGenerator(asset::MaterialAsset& material, const std::string& projectRoot)
{
    bool changed = false;
    ProceduralState& procedural = s_procedural;
    auto& particle = material.particle;
    ParticleFlipbookSettings& flipbook = particle.flipbook;

    static constexpr const char* kPresetNames[] = { "Smoke", "Fire", "Explosion", "Distortion" };
    ImGui::Combo("Preset", &procedural.preset, kPresetNames, IM_ARRAYSIZE(kPresetNames));
    ImGui::DragInt("Frame Size", &procedural.frameSize, 8.0f, 32, 512);
    ImGui::DragInt("Frames", &procedural.columns, 1.0f, 2, 32);
    ImGui::DragInt("Variants", &procedural.rows, 1.0f, 1, 16);
    ImGui::DragInt("Seed", &procedural.seed, 1.0f, 0, 1000000);
    ImGui::DragFloat("Noise Scale", &procedural.noiseScale, 0.05f, 0.25f, 32.0f);
    ImGui::DragFloat("Warp Strength", &procedural.warpStrength, 0.01f, 0.0f, 3.0f);
    const bool distortionPreset =
        procedural.preset == static_cast<int>(asset::ProceduralFlipbookPreset::Distortion);
    ImGui::BeginDisabled(distortionPreset);
    ImGui::Checkbox("Generate Motion Vectors", &procedural.generateMotionVectors);
    ImGui::EndDisabled();
    if (distortionPreset)
        ImGui::TextDisabled("Distortion は RG 自体が変位なので Motion Vector を生成しません。");
    ImGui::TextDisabled("生成した PNG をこの .mat の albedo へ割り当て、\n"
                        "プリセットに対応するブレンドとフリップブック設定を焼きます。");

    if (ImGui::Button("Generate & Assign", { -1.0f, 0.0f })) {
        asset::ProceduralFlipbookSettings settings;
        settings.preset       = static_cast<asset::ProceduralFlipbookPreset>(procedural.preset);
        settings.frameSize    = procedural.frameSize;
        settings.columns      = procedural.columns;
        settings.rows         = procedural.rows;
        settings.seed         = static_cast<std::uint32_t>((std::max)(procedural.seed, 0));
        settings.noiseScale   = procedural.noiseScale;
        settings.warpStrength = procedural.warpStrength;
        const auto result = asset::GenerateProceduralFlipbook(
            ProceduralVFXOutputDirectory(projectRoot), settings);
        procedural.status        = result.message;
        procedural.statusIsError = !result.success;
        if (result.success) {
            material.textures["albedo"]     = NormalizeAssetPath(result.albedoPath);
            flipbook.spriteColumns          = settings.columns;
            flipbook.spriteRows             = settings.rows;
            flipbook.spriteStartFrame       = 0;
            flipbook.spriteEndFrame         = settings.columns * settings.rows - 1;
            flipbook.flipbookFrameBlending  = true;
            flipbook.spriteRandomStartFrame = false;
            flipbook.spriteRandomRow        = settings.rows > 1;
            flipbook.motionVectorFlipbook   = false;
            material.textures["tex5"].clear();

            if (distortionPreset) {
                flipbook.flipbookMode            = ParticleFlipbookMode::FramesPerSecond;
                flipbook.flipbookFramesPerSecond = 24.0f;
                material.blendMode = renderer::BlendMode::ALPHA_BLEND;
                particle.distortion = true;
            } else {
                flipbook.flipbookMode = ParticleFlipbookMode::Lifetime;
                particle.distortion   = false;
                if (settings.preset == asset::ProceduralFlipbookPreset::Fire)
                    material.blendMode = renderer::BlendMode::ADDITIVE;
                else if (settings.preset == asset::ProceduralFlipbookPreset::Explosion)
                    material.blendMode = renderer::BlendMode::PREMULTIPLIED;
                else
                    material.blendMode = renderer::BlendMode::ALPHA_BLEND;

                if (procedural.generateMotionVectors) {
                    asset::FlipbookMotionVectorSettings mvSettings;
                    mvSettings.columns      = settings.columns;
                    mvSettings.rows         = settings.rows;
                    mvSettings.loop         = false;
                    mvSettings.rowSequences = settings.rows > 1;
                    const auto mvResult = asset::GenerateFlipbookMotionVectors(result.albedoPath, mvSettings);
                    if (mvResult.success) {
                        flipbook.motionVectorFlipbook = true;
                        material.textures["tex5"] = NormalizeAssetPath(mvResult.outputPath);
                        flipbook.motionVectorStrength = mvResult.recommendedStrength;
                        procedural.status += "\n" + mvResult.message;
                    } else {
                        procedural.status += "\nMV生成失敗: " + mvResult.message;
                        procedural.statusIsError = true;
                    }
                }
            }
            InvalidateAtlasPreview();
            changed = true;
        }
    }
    DrawStatus(procedural.status, procedural.statusIsError);
    return changed;
}

/// @name Motion Vector

bool DrawMotionVectorSection(asset::MaterialAsset& material, const std::string& projectRoot)
{
    bool changed = false;
    ParticleFlipbookSettings& flipbook = material.particle.flipbook;

    changed |= ImGui::Checkbox("Motion Vector Blending", &flipbook.motionVectorFlipbook);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("コマ間を速度場で warp してブレンドします。少ないコマ数でも滑らかに流れます。\n"
                          "有効にすると GPU シミュレーションは使えません (CPU へ縮退)。");
    if (!flipbook.motionVectorFlipbook) return changed;

    /// @note アトラスは [textures] tex5。ParticlePass が同じスロットから読む。
    changed |= widgets::AssetPathField("Motion Vector Atlas (tex5)", material.textures["tex5"],
                                       widgets::kTextureAssetFilter, projectRoot);
    changed |= ImGui::DragFloat("Motion Strength", &flipbook.motionVectorStrength, 0.0002f, 0.0f, 1.0f, "%.4f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MV アトラスの最大移動量 (Atlas UV)。生成時に自動で設定されます。\n"
                          "生成した値以外にすると、コマの補間が行き過ぎたり足りなくなったりします。");

    /// @note MV アトラスは外部ツールでしか作れず «機能はあるのに使えない» 状態だったため、
    ///       現在の albedo から生成してそのまま割り当てられるようにする。
    const std::string atlasPath = ResolveTextureDiskPath(projectRoot, TextureSlot(material, "albedo"));
    ImGui::BeginDisabled(atlasPath.empty());
    if (ImGui::Button("Generate From Texture", { -1.0f, 0.0f })) {
        asset::FlipbookMotionVectorSettings mvSettings;
        mvSettings.columns = flipbook.spriteColumns;
        mvSettings.rows    = flipbook.spriteRows;
        mvSettings.loop    = flipbook.flipbookMode == ParticleFlipbookMode::FramesPerSecond
            || flipbook.spriteRandomStartFrame;
        mvSettings.rowSequences = flipbook.spriteRandomRow;
        const auto result = asset::GenerateFlipbookMotionVectors(atlasPath, mvSettings);
        s_motionVectorStatus        = result.message;
        s_motionVectorStatusIsError = !result.success;
        if (result.success) {
            material.textures["tex5"] = NormalizeAssetPath(result.outputPath);
            flipbook.motionVectorStrength = result.recommendedStrength;
            /// @note Frame Blending が無いと spriteBlend が 0 のままで、MV が一切効かない。
            flipbook.flipbookFrameBlending = true;
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (atlasPath.empty()) ImGui::TextDisabled("albedo を設定すると生成できます。");
    DrawStatus(s_motionVectorStatus, s_motionVectorStatusIsError);
    return changed;
}

} // namespace

void InvalidateFlipbookAtlasPreview()
{
    InvalidateAtlasPreview();
}

bool DrawFlipbookInspector(asset::MaterialAsset& material, const std::string& projectRoot,
                           renderer::ResourceManager* resources,
                           renderer::IImGuiRenderer* imguiRenderer)
{
    ParticleFlipbookSettings& flipbook = material.particle.flipbook;
    bool changed = false;

    ImGui::PushID("flipbook");
    ImGui::SeparatorText("Texture Sheet Animation");

    const std::string& albedo = TextureSlot(material, "albedo");
    const AtlasPreview& atlas = ResolveAtlasPreview(albedo, resources, imguiRenderer);
    const std::string albedoDiskPath = ResolveTextureDiskPath(projectRoot, albedo);

    /// @name 概要とプレビュー
    DrawSummary(flipbook, asset::ResolveFlipbookRange(flipbook), atlas);

    if (s_preview.playing) s_preview.time += ImGui::GetIO().DeltaTime;
    /// @note 長時間開いたままでも float の精度が落ちないよう巻き戻す。
    if (s_preview.time > 3600.0f) s_preview.time = 0.0f;
    const asset::FlipbookFrameSample live = SamplePreview(flipbook, s_preview);

    if (atlas.textureId != nullptr || flipbook.FrameCount() > 1) {
        changed |= DrawAtlasGrid(flipbook, atlas, live);
        DrawFramePreview(atlas, asset::ResolveFlipbookRange(flipbook), live, ImGui::GetFontSize() * 7.0f);
        ImGui::SameLine();
        DrawPreviewControls(live);
    } else {
        ImGui::TextDisabled("albedo にアトラスを設定すると、コマ割りとアニメーションをここで確認できます。");
    }

    /// @name グリッド
    int grid[2] = { flipbook.spriteColumns, flipbook.spriteRows };
    if (ImGui::DragInt2("Columns / Rows", grid, 0.1f, 1, 64)) {
        flipbook.spriteColumns = std::clamp(grid[0], 1, 64);
        flipbook.spriteRows    = std::clamp(grid[1], 1, 64);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(albedoDiskPath.empty());
    if (ImGui::SmallButton("Detect")) {
        const asset::TextureAnalysis analysis = asset::AnalyzeTexture(albedoDiskPath);
        s_gridDetect            = {};
        s_gridDetect.candidates = analysis.flipbookCandidates;
        if (!analysis.success) {
            s_gridDetect.status        = analysis.message;
            s_gridDetect.statusIsError = true;
        } else if (s_gridDetect.candidates.empty()) {
            s_gridDetect.status = "コマ割りは見つかりませんでした (1 枚絵として扱います)。";
        } else {
            ImGui::OpenPopup("##gridCandidates");
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("albedo のタイル境界の不連続からコマ割りを推定します。");
    if (ImGui::BeginPopup("##gridCandidates")) {
        ImGui::TextDisabled("Candidates (best first)");
        for (const asset::FlipbookGridCandidate& candidate : s_gridDetect.candidates) {
            char label[96];
            std::snprintf(label, sizeof(label), "%d x %d    seam %.2f   uniformity %.2f",
                          candidate.columns, candidate.rows, candidate.seamScore, candidate.uniformity);
            if (ImGui::Selectable(label)) {
                flipbook.spriteColumns    = (std::max)(candidate.columns, 1);
                flipbook.spriteRows       = (std::max)(candidate.rows, 1);
                flipbook.spriteStartFrame = 0;
                flipbook.spriteEndFrame   = 0;
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    DrawStatus(s_gridDetect.status, s_gridDetect.statusIsError);

    /// @name 再生範囲
    const asset::FlipbookFrameRange range = asset::ResolveFlipbookRange(flipbook);
    const int  frameCount = range.columns * range.rows;
    const bool rowDriven  = flipbook.spriteRandomRow && range.rows > 1;
    ImGui::BeginDisabled(rowDriven || frameCount <= 1);
    bool toLast = flipbook.spriteEndFrame <= 0;
    if (ImGui::Checkbox("Play To Last Frame", &toLast)) {
        flipbook.spriteEndFrame = toLast ? 0 : (std::max)(range.last, 1);
        changed = true;
    }
    if (toLast) {
        changed |= ImGui::DragInt("Start Frame", &flipbook.spriteStartFrame, 0.1f, 0, frameCount - 1);
    } else {
        int first = flipbook.spriteStartFrame;
        int last  = flipbook.spriteEndFrame;
        if (ImGui::DragIntRange2("Frames", &first, &last, 0.1f, 0, frameCount - 1, "Start %d", "End %d")) {
            flipbook.spriteStartFrame = first;
            /// @note End = 0 は «最後まで» の意味になるので、範囲を明示している間は 1 以上に保つ。
            flipbook.spriteEndFrame = (std::max)(last, 1);
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (rowDriven) {
        char note[128];
        std::snprintf(note, sizeof(note),
                      "Random Row: 粒子ごとに 1 行を選び、その行の %d コマを再生します。", range.columns);
        DisabledWrapped(note);
    } else if (frameCount > 1) {
        ImGui::TextDisabled("アトラスをクリックで Start、右クリック / Shift+クリックで End。");
    }

    /// @name 再生のしかた
    static constexpr const char* kModeItems[] = { "Lifetime", "FPS", "Random", "Ping Pong" };
    int mode = static_cast<int>(flipbook.flipbookMode);
    if (ImGui::Combo("Mode", &mode, kModeItems, IM_ARRAYSIZE(kModeItems))) {
        flipbook.flipbookMode = static_cast<ParticleFlipbookMode>(mode);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lifetime : 寿命の長さで再生範囲を 1 回流す (煙・爆発)\n"
                          "FPS      : 一定の速さでループ (炎・電撃)\n"
                          "Random   : 粒子ごとに 1 コマを固定 (破片・バリエーション)\n"
                          "Ping Pong: 往復再生 (明滅・脈動)");
    if (IsTimedMode(flipbook.flipbookMode))
        changed |= ImGui::DragFloat("FPS", &flipbook.flipbookFramesPerSecond, 0.1f, 0.0f, 240.0f, "%.1f");

    changed |= ImGui::Checkbox("Frame Blending", &flipbook.flipbookFrameBlending);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("隣接コマを線形補間します。\n"
                          "有効にすると GPU シミュレーションは使えません (CPU へ縮退)。");
    changed |= ImGui::Checkbox("Random Start Frame", &flipbook.spriteRandomStartFrame);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("粒子ごとに再生位相をずらします。\n"
                          "同時に湧いた煙が全部同じコマで回って一枚板に見えるのを防ぎます。");
    ImGui::BeginDisabled(range.rows <= 1 && !flipbook.spriteRandomRow);
    changed |= ImGui::Checkbox("Random Row", &flipbook.spriteRandomRow);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("アトラスの各行を別バリエーションとして扱い、粒子ごとに 1 行を選びます。\n"
                          "1 枚のアトラスで見た目の異なる煙・爆炎を混ぜられます。\n"
                          "有効時は Start / End Frame より行の範囲が優先されます (行が 2 本以上必要)。");

    changed |= DrawFlipbookIssues(flipbook, material, atlas);

    /// @name Motion Vector
    changed |= DrawMotionVectorSection(material, projectRoot);

    /// @name アトラスを作る
    if (ImGui::TreeNode("Bake Atlas From Image Sequence")) {
        changed |= DrawSequenceBaker(material, projectRoot);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Procedural Flipbook Generator")) {
        changed |= DrawProceduralGenerator(material, projectRoot);
        ImGui::TreePop();
    }

    ImGui::PopID();
    return changed;
}

} // namespace fbzz::editor
