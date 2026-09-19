/// @file    MaterialPreview.cpp
/// @brief   Material 専用オフスクリーンプレビューのウィジェット。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// 選択中の .mat を AssetBrowser のサムネイルと同じ MaterialPreviewCore で焼き、形状・背景・
/// 照明・表示チャンネルを切り替えられる形で出す。Inspector と Preview パネルの見た目がアセットの
/// 場所で変わらないようにする。
#include <Editor/Panels/MaterialPreview.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace fbzz::editor {

namespace {

namespace mp = matpreview;

constexpr int kPreviewRtSize = 512;

ImTextureID ToImTextureID(void* ptr)
{
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

constexpr std::array<mp::Shape, 7> kShapes = {
    mp::Shape::Sphere, mp::Shape::Cube, mp::Shape::Plane, mp::Shape::Cylinder,
    mp::Shape::Cone,   mp::Shape::Torus, mp::Shape::Capsule,
};

constexpr std::array<mp::Channel, 9> kChannels = {
    mp::Channel::Shaded,    mp::Channel::Albedo,    mp::Channel::Normal,
    mp::Channel::Roughness, mp::Channel::Metallic,  mp::Channel::Occlusion,
    mp::Channel::Emissive,  mp::Channel::Uv,        mp::Channel::Wireframe,
};

constexpr std::array<mp::FiberMode, 4> kFiberModes = {
    mp::FiberMode::Shell, mp::FiberMode::Fin, mp::FiberMode::Hybrid, mp::FiberMode::Blade,
};

constexpr std::array<mp::LightPreset, 4> kPresets = {
    mp::LightPreset::Studio, mp::LightPreset::Outdoor,
    mp::LightPreset::Night,  mp::LightPreset::Flat,
};

constexpr std::array<MaterialPreviewBackground, 4> kBackgrounds = {
    MaterialPreviewBackground::Gradient, MaterialPreviewBackground::Checker,
    MaterialPreviewBackground::Solid,    MaterialPreviewBackground::Grid,
};

const char* BackgroundLabel(MaterialPreviewBackground background)
{
    switch (background) {
        case MaterialPreviewBackground::Checker: return "Checker";
        case MaterialPreviewBackground::Solid:   return "Solid";
        case MaterialPreviewBackground::Grid:    return "Grid";
        case MaterialPreviewBackground::Gradient:
        default:                                 return "Gradient";
    }
}

void DrawFrameBorder(ImDrawList* drawList, ImVec2 origin, float size)
{
    drawList->AddRect(origin, { origin.x + size, origin.y + size },
                      IM_COL32(95, 100, 112, 230), 4.0f, 0, 1.0f);
}

void DrawChecker(ImDrawList* drawList, ImVec2 origin, float size)
{
    constexpr float kCell = 12.0f;
    drawList->AddRectFilled(origin, { origin.x + size, origin.y + size },
                            IM_COL32(58, 60, 66, 255), 4.0f);
    drawList->PushClipRect(origin, { origin.x + size, origin.y + size }, true);
    const int cells = static_cast<int>(std::ceil(size / kCell));
    for (int y = 0; y < cells; ++y) {
        for (int x = (y & 1); x < cells; x += 2) {
            const ImVec2 cellMin{ origin.x + x * kCell, origin.y + y * kCell };
            drawList->AddRectFilled(
                cellMin,
                { std::min(cellMin.x + kCell, origin.x + size),
                  std::min(cellMin.y + kCell, origin.y + size) },
                IM_COL32(40, 42, 47, 255));
        }
    }
    drawList->PopClipRect();
}

void DrawGrid(ImDrawList* drawList, ImVec2 origin, float size)
{
    constexpr float kCell = 16.0f;
    drawList->AddRectFilled(origin, { origin.x + size, origin.y + size },
                            IM_COL32(24, 26, 30, 255), 4.0f);
    drawList->PushClipRect(origin, { origin.x + size, origin.y + size }, true);
    for (float offset = kCell; offset < size; offset += kCell) {
        drawList->AddLine({ origin.x + offset, origin.y }, { origin.x + offset, origin.y + size },
                          IM_COL32(48, 52, 60, 255));
        drawList->AddLine({ origin.x, origin.y + offset }, { origin.x + size, origin.y + offset },
                          IM_COL32(48, 52, 60, 255));
    }
    const float center = std::floor(size * 0.5f / kCell) * kCell;
    drawList->AddLine({ origin.x + center, origin.y }, { origin.x + center, origin.y + size },
                      IM_COL32(74, 80, 92, 255));
    drawList->AddLine({ origin.x, origin.y + center }, { origin.x + size, origin.y + center },
                      IM_COL32(74, 80, 92, 255));
    drawList->PopClipRect();
}

/// AssetBrowser の DrawThumbnailFrame と同じ背景グラデーション。
void DrawGradient(ImDrawList* drawList, ImVec2 origin, float size, bool hovered)
{
    const ImU32 base = hovered ? IM_COL32(42, 45, 52, 255) : IM_COL32(30, 32, 38, 255);
    drawList->AddRectFilled(origin, { origin.x + size, origin.y + size }, base, 4.0f);
    const ImU32 gradTop    = hovered ? IM_COL32(56, 60, 70, 255) : IM_COL32(44, 47, 56, 255);
    const ImU32 gradBottom = hovered ? IM_COL32(30, 32, 38, 255) : IM_COL32(19, 20, 24, 255);
    /// @note AddRectFilledMultiColor は角丸非対応なので 2px 内側へ重ね、角丸の輪郭を残す。
    drawList->AddRectFilledMultiColor(
        { origin.x + 2.0f, origin.y + 2.0f },
        { origin.x + size - 2.0f, origin.y + size - 2.0f },
        gradTop, gradTop, gradBottom, gradBottom);
}

void DrawBadge(ImDrawList* drawList, ImVec2 origin, float size, const char* badge)
{
    const ImVec2 textSize = ImGui::CalcTextSize(badge);
    const ImVec2 badgeMin{ origin.x + size - textSize.x - 12.0f, origin.y + size - textSize.y - 7.0f };
    const ImVec2 badgeMax{ origin.x + size - 4.0f, origin.y + size - 3.0f };
    drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(20, 22, 26, 205), 3.0f);
    drawList->AddText({ badgeMin.x + 4.0f, badgeMin.y + 2.0f }, IM_COL32(235, 240, 245, 230), badge);
}

} // namespace

void MaterialPreviewView::ResetView()
{
    m_orbit   = {};
    m_rig     = {};
    m_channel = mp::Channel::Shaded;
}

void MaterialPreviewView::DrawToolbar(EditorContext& ctx, mp::Flavor flavor)
{
    (void)ctx;
    const float full = (std::max)(ImGui::GetContentRegionAvail().x, 120.0f);
    const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    /// @note Terrain / Water は «地面» と «水面»、UI は矩形、エフェクト系はビルボードや
    ///       リボンと、形そのものが素材の一部なので選ばせない。
    const bool shapeLocked = flavor == mp::Flavor::Terrain ||
                             flavor == mp::Flavor::Water ||
                             mp::UsesOwnGeometry(flavor);
    const char* lockedLabel =
        flavor == mp::Flavor::Ui          ? "Rect"     :
        flavor == mp::Flavor::Particle    ? "Billboard":
        flavor == mp::Flavor::Trail       ? "Ribbon"   :
        flavor == mp::Flavor::Decal       ? "Projected":
        flavor == mp::Flavor::PostProcess ? "Fullscreen" : "Plane";

    ImGui::BeginDisabled(shapeLocked);
    ImGui::SetNextItemWidth(half);
    if (ImGui::BeginCombo("##Shape", shapeLocked ? lockedLabel : mp::ShapeLabel(m_shape))) {
        for (mp::Shape shape : kShapes) {
            if (ImGui::Selectable(mp::ShapeLabel(shape), shape == m_shape)) m_shape = shape;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (shapeLocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("This material type is previewed on its own geometry.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    if (ImGui::BeginCombo("##Channel", mp::ChannelLabel(m_channel))) {
        for (mp::Channel channel : kChannels) {
            const bool supported = mp::ChannelSupported(flavor, channel);
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(mp::ChannelLabel(channel), channel == m_channel) && supported)
                m_channel = channel;
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    /// @note 対応していないチャンネルのまま材質が切り替わることがあるので毎フレーム畳む。
    if (!mp::ChannelSupported(flavor, m_channel)) m_channel = mp::Channel::Shaded;

    ImGui::SetNextItemWidth(half);
    if (ImGui::BeginCombo("##Light", mp::LightPresetLabel(m_rig.preset))) {
        for (mp::LightPreset preset : kPresets) {
            if (ImGui::Selectable(mp::LightPresetLabel(preset), preset == m_rig.preset))
                m_rig.preset = preset;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    if (ImGui::BeginCombo("##Background", BackgroundLabel(m_background))) {
        for (MaterialPreviewBackground background : kBackgrounds) {
            if (ImGui::Selectable(BackgroundLabel(background), background == m_background))
                m_background = background;
        }
        ImGui::EndCombo();
    }

    if (m_background == MaterialPreviewBackground::Solid) {
        ImGui::SetNextItemWidth(full);
        ImGui::ColorEdit3("##BackgroundColor", m_solidColor,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    }

    ImGui::SetNextItemWidth(half);
    ImGui::SliderFloat("##Exposure", &m_rig.exposure, 0.1f, 4.0f, "EV %.2f");
    if (flavor == mp::Flavor::Water || flavor == mp::Flavor::Fiber) {
        ImGui::SameLine();
        ImGui::Checkbox("Animate", &m_animateWater);
    }

    if (flavor == mp::Flavor::Fiber) {
        ImGui::SetNextItemWidth(half);
        if (ImGui::BeginCombo("##FiberMode", mp::FiberModeLabel(m_fiberMode))) {
            for (mp::FiberMode mode : kFiberModes) {
                if (ImGui::Selectable(mp::FiberModeLabel(mode), mode == m_fiberMode)) m_fiberMode = mode;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fiber render mode. The .mat is shared by all modes; the Fiber component picks one.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half);
        ImGui::BeginDisabled(m_fiberMode == mp::FiberMode::Fin || m_fiberMode == mp::FiberMode::Blade);
        ImGui::SliderInt("##FiberShells", &m_fiberShellCount, 1, 64, "Shells %d");
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(full);
        ImGui::SliderFloat("##FiberWind", &m_fiberWind, 0.0f, 10.0f, "Wind %.1f m/s");
    }
}

void MaterialPreviewView::DrawBackground(ImVec2 origin, float size, bool hovered) const
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    switch (m_background) {
        case MaterialPreviewBackground::Checker: DrawChecker(drawList, origin, size); break;
        case MaterialPreviewBackground::Grid:    DrawGrid(drawList, origin, size);    break;
        case MaterialPreviewBackground::Solid:
            drawList->AddRectFilled(origin, { origin.x + size, origin.y + size },
                ImGui::ColorConvertFloat4ToU32({ m_solidColor[0], m_solidColor[1], m_solidColor[2], 1.0f }),
                4.0f);
            break;
        case MaterialPreviewBackground::Gradient:
        default: DrawGradient(drawList, origin, size, hovered); break;
    }
    DrawFrameBorder(drawList, origin, size);
}

void MaterialPreviewView::DrawUnsupported(EditorContext& ctx,
                                          const asset::MaterialAsset& material,
                                          ImVec2 origin,
                                          float size)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const char* badge = mp::UnsupportedBadge(material);

    /// @note 素材があればその絵を出す。«この材質はこういう色» なのか «焼けなかった» のかを
    ///       区別できるよう、色見本だけで終わらせない。
    const std::string texturePath = mp::RepresentativeTexturePath(material);
    if (texturePath != m_fallbackTexturePath) {
        m_fallbackTexturePath = texturePath;
        m_fallbackTexture = {};
        m_fallbackTextureResolved = false;
    }
    if (!m_fallbackTextureResolved && !texturePath.empty() && ctx.resources) {
        m_fallbackTexture = ctx.resources->LoadTexture(
            mp::TextureLoadPath(texturePath, ctx.projectRoot));
        m_fallbackTextureResolved = true;
    }

    if (m_fallbackTexture.IsValid() && ctx.resources && ctx.imguiRenderer) {
        if (void* rawId = ctx.imguiRenderer->GetImTextureID(m_fallbackTexture, *ctx.resources)) {
            const auto* texture = ctx.resources->Get(m_fallbackTexture);
            const float width  = texture ? static_cast<float>((std::max)(1u, texture->GetWidth()))  : 1.0f;
            const float height = texture ? static_cast<float>((std::max)(1u, texture->GetHeight())) : 1.0f;
            const float scale = (std::min)((size - 16.0f) / width, (size - 16.0f) / height);
            const ImVec2 imageSize{ (std::max)(1.0f, width * scale), (std::max)(1.0f, height * scale) };
            const ImVec2 imageMin{ origin.x + (size - imageSize.x) * 0.5f,
                                   origin.y + (size - imageSize.y) * 0.5f };
            drawList->AddImage(ToImTextureID(rawId), imageMin,
                               { imageMin.x + imageSize.x, imageMin.y + imageSize.y });
            DrawBadge(drawList, origin, size, badge);
            return;
        }
    }

    const math::Vector4 color = mp::SwatchColor(material);
    const ImU32 top    = ImGui::ColorConvertFloat4ToU32({ color.x, color.y, color.z, 1.0f });
    const ImU32 bottom = ImGui::ColorConvertFloat4ToU32(
        { color.x * 0.35f, color.y * 0.35f, color.z * 0.35f, 1.0f });
    const float inset = size * 0.18f;
    const ImVec2 chipMin{ origin.x + inset, origin.y + inset };
    const ImVec2 chipMax{ origin.x + size - inset, origin.y + size - inset };
    drawList->AddRectFilledMultiColor(chipMin, chipMax, top, top, bottom, bottom);
    drawList->AddRect(chipMin, chipMax, IM_COL32(20, 22, 26, 180), 0.0f, 0, 1.0f);
    DrawBadge(drawList, origin, size, badge);
}

bool MaterialPreviewView::RenderFrame(EditorContext& ctx,
                                      const asset::MaterialAsset& material,
                                      mp::Flavor flavor)
{
    if (!ctx.renderer || !ctx.resources) return false;
    auto& resources = *ctx.resources;

    if (!m_renderTarget.IsValid())
        m_renderTarget = resources.CreateRenderTarget(kPreviewRtSize, kPreviewRtSize);
    if (!m_renderTarget.IsValid()) return false;

    /// @note 毎フレーム作り直す: Inspector はスライダーを動かしている最中の .mat をそのまま渡してくる。
    ///       保存を待つと «動かしても絵が変わらない» になる。LoadShader / LoadTexture はキャッシュに当たる。
    if (!mp::BuildGpuData(m_gpu, material, resources, ctx.projectRoot)) return false;

    mp::RenderDesc desc;
    desc.target   = m_renderTarget;
    desc.flavor   = flavor;
    desc.material = &material;
    desc.gpu      = &m_gpu;
    desc.channel  = m_channel;
    desc.rig      = m_rig;
    desc.orbit    = m_orbit;
    desc.time     = m_animateWater ? static_cast<float>(ImGui::GetTime()) : 0.0f;
    desc.fiberMode       = m_fiberMode;
    desc.fiberShellCount = m_fiberShellCount;
    desc.fiberWind       = m_fiberWind;
    desc.mesh     = mp::ShapeMesh(resources, m_shape, flavor);
    if (!desc.mesh && !mp::UsesOwnGeometry(flavor)) return false;

    return mp::Render(*ctx.renderer, resources, desc);
}

bool MaterialPreviewView::Draw(EditorContext& ctx,
                               const asset::MaterialAsset& material,
                               float previewHeight)
{
    ImGui::PushID(this);

    const mp::Flavor flavor = mp::DetectFlavor(material);
    DrawToolbar(ctx, flavor);

    /// @note AssetBrowser と同じ正方形表示にする。横長の Inspector 幅へ引き伸ばすと、
    ///       同じ RT でも球の投影とハイライトの位置が別物に見えるため。
    const float width  = (std::max)(ImGui::GetContentRegionAvail().x, 64.0f);
    const float height = (std::max)(previewHeight, 96.0f);
    const float size   = (std::max)((std::min)(width, height), 64.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##Image", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    if (hovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        /// @note Ctrl 併用で «光だけ» を回す。Unity / Blender のマテリアルプレビューと同じ操作。
        if (ImGui::GetIO().KeyCtrl) {
            m_rig.yaw -= delta.x * 0.012f;
            m_rig.pitch = std::clamp(m_rig.pitch + delta.y * 0.010f, -1.35f, 1.35f);
        } else {
            m_orbit.yaw -= delta.x * 0.012f;
            m_orbit.pitch = std::clamp(m_orbit.pitch + delta.y * 0.010f, -1.35f, 1.35f);
        }
    }
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        m_orbit.distance *= ImGui::GetIO().MouseWheel > 0.0f ? 0.88f : 1.14f;
        m_orbit.distance = std::clamp(m_orbit.distance, 1.5f, 8.0f);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ResetView();

    DrawBackground(origin, size, hovered);

    if (flavor == mp::Flavor::Unsupported) {
        /// @note 3D へは焼けないが «何の素材か» は出している。呼び出し側に
        ///       «プレビュー無し» のプレースホルダーを重ねさせないため true を返す。
        DrawUnsupported(ctx, material, origin, size);
        ImGui::PopID();
        return true;
    }

    const bool rendered = RenderFrame(ctx, material, flavor);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    void* rawId = (rendered && ctx.imguiRenderer && ctx.resources)
        ? ctx.imguiRenderer->GetImTextureID(m_renderTarget, *ctx.resources, 0)
        : nullptr;
    if (rawId) {
        drawList->AddImage(ToImTextureID(rawId), origin, { origin.x + size, origin.y + size });
    } else {
        /// @note 焼けるはずの Flavor なのに失敗した (シェーダーが壊れている等)。
        ///       «何も出ない» で終わらせず、素材と種別だけは出す。
        DrawUnsupported(ctx, material, origin, size);
    }

    if (hovered) {
        drawList->AddText({ origin.x + 8.0f, origin.y + size - ImGui::GetTextLineHeight() - 6.0f },
                          IM_COL32(170, 178, 190, 210),
                          "Drag: Orbit | Ctrl+Drag: Light | Wheel: Zoom | Dbl: Reset");
    }

    ImGui::PopID();
    /// @note 焼けなくても色見本は出しているので «描いた» と返す。呼び出し側に
    ///       «プレビュー無し» のプレースホルダーを重ねさせないため。
    return true;
}

} // namespace fbzz::editor
