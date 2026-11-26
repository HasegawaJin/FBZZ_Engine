/// @file    MaterialPreview.cpp
/// @brief   Material 専用オフスクリーンプレビューのウィジェット。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#include "MaterialPreviewChoices.hpp"
#include <Editor/UI/Localization.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialParamBinding.hpp>
#include <Graphics/Renderer/UI/IImGuiRenderer.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/Textures/ITexture.hpp>
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Resources/AssetPathService.hpp>
#include <Engine/Util/Files/FileSystem.hpp>

#include <imgui.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <vector>

namespace fbzz::editor {

namespace {

namespace mp = matpreview;

constexpr int kPreviewRtSize = 512;
constexpr double kNormalMapRetryInterval = 0.5;

struct TextureFileRevision {
    bool exists = false;
    std::int64_t writeTime = 0;
};

TextureFileRevision ReadTextureFileRevision(const std::string& path)
{
    const std::string resolvedAssetPath = renderer::ResolveAssetPath(path);
    if (resolvedAssetPath.empty()) {
        return {};
    }
    std::string sourcePath;
    if (!renderer::ResolveTextureSource(resolvedAssetPath, sourcePath) || sourcePath.empty()) {
        return {};
    }

    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(
        util::FileSystem::PathFromUtf8(sourcePath), error);
    if (error) {
        return {};
    }
    return { true, static_cast<std::int64_t>(writeTime.time_since_epoch().count()) };
}

mp::PreviewResourceKey ReadPreviewResourceKey(const EditorContext& ctx)
{
    mp::PreviewResourceKey key;
    if (ctx.resources) {
        const auto lifetime = mp::PreviewResourceLifetime::Read(*ctx.resources);
        key.instanceId = lifetime.instanceId;
        key.resetVersion = lifetime.resetVersion;
        key.shaderVersion = ctx.resources->GetShaderVersion();
    }
    key.assetGeneration = asset::AssetManager::GetAssetGeneration();
    key.projectRoot = ctx.projectRoot;
    return key;
}

mp::PreviewGpuKey ReadPreviewGpuKey(const asset::MaterialAssetAuthoring& material,
                                   renderer::ResourceManager& resources,
                                   const mp::PreviewResourceKey& resourceKey)
{
    mp::PreviewGpuKey key;
    key.resources = resourceKey;
    key.textures.reserve(material.textures.size() * 2);
    for (const auto& textureBinding : material.textures) {
        const auto& path = textureBinding.second;
        if (path.empty()) {
            continue;
        }
        const std::string loadPath = mp::TextureLoadPath(path, resourceKey.projectRoot);
        // 法線の raw RGB と G 反転版は独立した GPU キャッシュを持つ。
        for (bool flipGreen : { false, true }) {
            const auto handle = resources.FindCachedTexture(loadPath, flipGreen);
            const auto* texture = resources.Get(handle);
            key.textures.push_back({ handle, texture, texture ? texture->GetContentVersion() : 0 });
        }
    }
    // unordered_map の再配置だけでは未変更の材質を再描画しない。
    std::sort(key.textures.begin(), key.textures.end(), [](const auto& a, const auto& b) {
        return std::tie(a.handle.id, a.handle.gen) < std::tie(b.handle.id, b.handle.gen);
    });
    return key;
}

ImTextureID ToImTextureID(void* ptr)
{
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

void DrawChecker(ImDrawList& draw, ImVec2 min, ImVec2 max)
{
    const float cell = ImGui::GetFontSize();
    draw.AddRectFilled(min, max, IM_COL32(58, 60, 66, 255));
    int row = 0;
    for (float y = min.y; y < max.y; y += cell, ++row) {
        for (float x = min.x + (row & 1) * cell; x < max.x; x += 2.0f * cell)
            draw.AddRectFilled({ x, y }, { std::min(x + cell, max.x), std::min(y + cell, max.y) },
                               IM_COL32(40, 42, 47, 255));
    }
}

void DrawGrid(ImDrawList& draw, ImVec2 min, ImVec2 max)
{
    const float cell = ImGui::GetFrameHeight();
    const ImVec2 center{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f };
    draw.AddRectFilled(min, max, IM_COL32(24, 26, 30, 255));
    const ImU32 grid = IM_COL32(48, 52, 60, 255), axis = IM_COL32(74, 80, 92, 255);
    for (float x = min.x + std::fmod(center.x - min.x, cell); x < max.x; x += cell) {
        draw.AddLine({ x, min.y }, { x, max.y }, grid);
    }
    for (float y = min.y + std::fmod(center.y - min.y, cell); y < max.y; y += cell) {
        draw.AddLine({ min.x, y }, { max.x, y }, grid);
    }
    draw.AddLine({ center.x, min.y }, { center.x, max.y }, axis);
    draw.AddLine({ min.x, center.y }, { max.x, center.y }, axis);
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
    m_showParallax = true;
}

void MaterialPreviewView::RegisterOperators(OperatorRegistry& registry, std::string_view prefix)
{
    for (const std::string_view action : {"inspect", "configure", "reset"}) {
        EditorOperator op;
        op.id = std::string(prefix) + "." + std::string(action);
        op.label = op.id;
        op.category = "Preview";
        op.kind = action == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "材質プレビューの一時表示設定。configure は inspect のキーを settings に部分指定。角度はラジアン。";
        if (action == "configure") {
            OpParam settings;
            settings.name = "settings";
            settings.type = OpParamType::Data;
            op.params.push_back(std::move(settings));
        }
        op.exec = [this, action](OpContext&, const OpArgs& args) {
            const auto* settings = args.GetData("settings");
            if (action == "configure" && (!settings || !settings->IsObject())) {
                return OpResult::Err("BAD_ARG", "settings");
            }
            if (action == "reset") {
                ResetView();
            }

            auto data = OpData::MakeObject();
            std::vector<std::function<void()>> changes;
            std::string invalid;
            const auto field = [&](const char* key, auto& target, auto encode, auto decode) {
                if (!settings) {
                    data.Set(key, encode(target));
                    return;
                }
                const auto* value = settings->Find(key);
                if (!value) {
                    return;
                }
                auto next = target;
                if (!decode(*value, next)) {
                    invalid = key;
                    return;
                }
                changes.emplace_back([&target, next] { target = next; });
            };
            const auto choice = [&](const char* key, auto& target, const auto& values, auto label) {
                field(key, target, [&](auto value) { return OpData(label(value)); },
                    [&](const OpData& input, auto& next) {
                        if (!input.IsString()) {
                            return false;
                        }
                        const auto match = std::find_if(values.begin(), values.end(),
                            [&](auto value) { return input.AsString() == label(value); });
                        if (match == values.end()) {
                            return false;
                        }
                        next = *match;
                        return true;
                    });
            };
            const auto number = [&](const char* key, auto& target, double minimum, double maximum) {
                field(key, target, [](auto value) { return OpData(value); },
                    [&](const OpData& input, auto& next) {
                        using Number = std::remove_reference_t<decltype(next)>;
                        if (!input.IsInt() && (std::is_integral_v<Number> || !input.IsFloat())) {
                            return false;
                        }
                        const double value = input.IsInt() ? static_cast<double>(input.AsInt64()) : input.AsDouble();
                        if (!std::isfinite(value) || value < minimum || value > maximum) {
                            return false;
                        }
                        next = static_cast<Number>(value);
                        return true;
                    });
            };
            choice("shape", m_shape, previewchoices::SHAPES, mp::ShapeLabel);
            choice("channel", m_channel, previewchoices::CHANNELS, mp::ChannelLabel);
            choice("light", m_rig.preset, previewchoices::LIGHT_PRESETS, mp::LightPresetLabel);
            choice("background", m_background, previewchoices::BACKGROUNDS, previewchoices::BackgroundLabel);
            choice("fiberMode", m_fiberMode, previewchoices::FIBER_MODES, mp::FiberModeLabel);
            number("backgroundRed", m_solidColor[0], 0, 1);
            number("backgroundGreen", m_solidColor[1], 0, 1);
            number("backgroundBlue", m_solidColor[2], 0, 1);
            number("exposure", m_rig.exposure, 0.1, 4);
            number("lightYaw", m_rig.yaw, -100000, 100000);
            number("lightPitch", m_rig.pitch, -1.35, 1.35);
            number("yaw", m_orbit.yaw, -100000, 100000);
            number("pitch", m_orbit.pitch, -1.35, 1.35);
            number("distance", m_orbit.distance, 1.5, 8);
            number("fiberShells", m_fiberShellCount, 1, 64);
            number("fiberWind", m_fiberWind, 0, 10);
            const auto boolean = [&](const char* key, bool& target) {
                field(key, target, [](bool value) { return OpData(value); },
                    [](const OpData& input, bool& next) {
                        if (!input.IsBool()) {
                            return false;
                        }
                        next = input.AsBool();
                        return true;
                    });
            };
            boolean("animate", m_animateWater);
            boolean("parallax", m_showParallax);
            if (!invalid.empty()) {
                return OpResult::Err("BAD_ARG", invalid);
            }
            if (!settings) {
                return OpResult::Data(std::move(data));
            }
            if (changes.size() != settings->AsObject().size()) {
                return OpResult::Err("BAD_ARG", "settings に不明なキーがあります。");
            }
            for (const auto& change : changes) {
                change();
            }
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }
}

void MaterialPreviewView::SynchronizeResources(EditorContext& ctx)
{
    const auto key = ReadPreviewResourceKey(ctx);
    const bool managerChanged = key.Lifetime() != m_resourceKey.Lifetime();
    const bool projectChanged = key.projectRoot != m_resourceKey.projectRoot;
    const bool shaderChanged = key.shaderVersion != m_resourceKey.shaderVersion;
    if (managerChanged || projectChanged || shaderChanged) {
        // 旧 Manager / Reset 前のハンドルを新しい資源プールへ返さない。
        InvalidateGpu(managerChanged ? nullptr : ctx.resources);
        if (managerChanged) {
            m_renderTarget = {};
        }
    }
    if (managerChanged || projectChanged || key.assetGeneration != m_resourceKey.assetGeneration) {
        m_fallbackTexture = {};
        m_fallbackTexturePath.clear();
        m_fallbackTextureResolved = false;
        m_normalMapTexture = {};
        m_normalMapTexturePath.clear();
        m_normalMapTextureResolved = false;
        m_normalMapNextFileCheck = 0.0;
    }
    m_resourceKey = key;
}

void MaterialPreviewView::InvalidateGpu(renderer::ResourceManager* resources)
{
    mp::ResetGpuData(m_gpu, resources);
    m_gpuCache.Invalidate();
    m_frameCache.Invalidate();
}

void MaterialPreviewView::DrawBackground(ImVec2 min, ImVec2 max) const
{
    auto& draw = *ImGui::GetWindowDrawList();
    switch (m_background) {
        case MaterialPreviewBackground::Checker: DrawChecker(draw, min, max); break;
        case MaterialPreviewBackground::Grid:    DrawGrid(draw, min, max);    break;
        case MaterialPreviewBackground::Solid:
            draw.AddRectFilled(min, max, ImGui::GetColorU32({ m_solidColor[0], m_solidColor[1], m_solidColor[2], 1.0f }));
            break;
        case MaterialPreviewBackground::Gradient: {
            const ImU32 top = IM_COL32(44, 47, 56, 255), bottom = IM_COL32(19, 20, 24, 255);
            draw.AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
            break;
        }
    }
}

void MaterialPreviewView::DrawNormalMap(EditorContext& ctx,
                                        const asset::MaterialAssetAuthoring& material)
{
    const auto texturePaths = asset::ResolveMaterialTexturePaths(material);
    const std::string& texturePath = texturePaths[1];
    if (texturePath.empty()) {
        m_normalMapTexture = {};
        m_normalMapTexturePath.clear();
        m_normalMapTextureResolved = false;
        m_normalMapFileExists = false;
        m_normalMapFileRevision = 0;
        m_normalMapNextFileCheck = 0.0;
        return;
    }

    const std::string loadPath = mp::TextureLoadPath(texturePath, ctx.projectRoot);
    if (loadPath != m_normalMapTexturePath) {
        m_normalMapTexture = {};
        m_normalMapTexturePath = loadPath;
        m_normalMapTextureResolved = false;
        m_normalMapFileExists = false;
        m_normalMapFileRevision = 0;
        m_normalMapNextFileCheck = ImGui::GetTime();
    }

    const double now = ImGui::GetTime();
    if (ctx.resources && m_normalMapTextureResolved && !m_normalMapTexture.IsValid() &&
        now >= m_normalMapNextFileCheck) {
        const TextureFileRevision revision = ReadTextureFileRevision(loadPath);
        if (revision.exists != m_normalMapFileExists ||
            (revision.exists && revision.writeTime != m_normalMapFileRevision)) {
            m_normalMapFileExists = revision.exists;
            m_normalMapFileRevision = revision.writeTime;
            m_normalMapTextureResolved = false;
        }
        m_normalMapNextFileCheck = now + kNormalMapRetryInterval;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Normal map (raw RGB)");
    ImGui::TextWrapped("%s", texturePath.c_str());

    if (!ctx.resources) {
        ImGui::TextWrapped("Normal map preview unavailable: renderer resources are unavailable.");
        return;
    }
    if (!m_normalMapTextureResolved) {
        const TextureFileRevision revision = ReadTextureFileRevision(loadPath);
        m_normalMapTexture = ctx.resources->LoadTexture(loadPath);
        m_normalMapFileExists = revision.exists;
        m_normalMapFileRevision = revision.writeTime;
        m_normalMapTextureResolved = true;
        m_normalMapNextFileCheck = now + kNormalMapRetryInterval;
    }
    if (!m_normalMapTexture.IsValid()) {
        ImGui::TextWrapped("Failed to load the normal map texture. Check the asset path and image format.");
        return;
    }
    if (!ctx.imguiRenderer) {
        ImGui::TextWrapped("Normal map loaded, but the preview renderer is unavailable.");
        return;
    }

    void* rawId = ctx.imguiRenderer->GetImTextureID(m_normalMapTexture, *ctx.resources);
    if (!rawId) {
        ImGui::TextWrapped("Normal map loaded, but its GPU view is unavailable.");
        return;
    }

    const auto* texture = ctx.resources->Get(m_normalMapTexture);
    const float width = texture ? static_cast<float>((std::max)(1u, texture->GetWidth())) : 1.0f;
    const float height = texture ? static_cast<float>((std::max)(1u, texture->GetHeight())) : 1.0f;
    const float maxSize = (std::max)(1.0f, (std::min)(192.0f, ImGui::GetContentRegionAvail().x));
    const float scale = (std::min)(maxSize / width, maxSize / height);
    const ImVec2 imageSize{ (std::max)(1.0f, width * scale),
                            (std::max)(1.0f, height * scale) };
    ImGui::Image(ToImTextureID(rawId), imageSize);
}

void MaterialPreviewView::DrawUnsupported(EditorContext& ctx,
                                          const asset::MaterialAssetAuthoring& material,
                                          ImVec2 origin,
                                          float size)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const char* badge = mp::UnsupportedBadge(material);

    // 代表画像またはスウォッチで描画失敗と材質色を見分ける。
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

bool MaterialPreviewView::EnsureGpuData(EditorContext& ctx, const asset::MaterialAssetAuthoring& material)
{
    if (!ctx.renderer || !ctx.resources) {
        return false;
    }
    auto& resources = *ctx.resources;

    if ((m_gpu.shader.IsValid() && !resources.Get(m_gpu.shader))
        || (m_gpu.materialCB.IsValid() && !resources.Get(m_gpu.materialCB))) {
        InvalidateGpu(&resources);
    }
    const auto gpuKey = ReadPreviewGpuKey(material, resources, m_resourceKey);
    if (!m_gpuCache.IsCurrent(gpuKey) || !mp::MaterialPreviewValuesEqual(m_sourceMaterial, material)) {
        m_frameCache.Invalidate();
        if (!mp::BuildGpuData(m_gpu, material, resources, ctx.projectRoot)) {
            m_gpuCache.Invalidate();
            return false;
        }
        m_sourceMaterial = material;
        // BuildGpuData が初めてロードしたテクスチャも成功済み入力へ含める。
        m_gpuCache.Commit(ReadPreviewGpuKey(material, resources, m_resourceKey));
        ++m_sourceRevision;
    }
    return true;
}

bool MaterialPreviewView::EnsurePreviewFrame(EditorContext& ctx,
                                             const asset::MaterialAssetAuthoring& material,
                                             mp::Flavor flavor)
{
    auto& resources = *ctx.resources;
    if (m_renderTarget.IsValid() && !resources.Get(m_renderTarget)) {
        m_renderTarget = {};
        m_frameCache.Invalidate();
    }
    if (!m_renderTarget.IsValid())
        m_renderTarget = resources.CreateRenderTarget(kPreviewRtSize, kPreviewRtSize,
                                                      renderer::CameraDepthTargetDesc(1));
    if (!m_renderTarget.IsValid()) {
        return false;
    }

    mp::RenderDesc desc;
    desc.target   = m_renderTarget;
    desc.flavor   = flavor;
    desc.material = std::make_shared<const asset::MaterialAssetAuthoring>(material);
    desc.gpu      = &m_gpu;
    desc.channel  = m_channel;
    desc.showParallax = m_showParallax;
    desc.rig      = m_rig;
    desc.orbit    = m_orbit;
    desc.fiberMode       = m_fiberMode;
    desc.fiberShellCount = m_fiberShellCount;
    desc.fiberWind       = m_fiberWind;
    const auto frameKey = mp::MakePreviewFrameKey(m_sourceRevision, desc, m_shape, m_animateWater);
    if (!mp::UsesPreviewTime(flavor, m_animateWater) && m_frameCache.IsCurrent(frameKey)) {
        return true;
    }

    m_frameCache.Invalidate();
    if (!RenderFrame(ctx, desc)) {
        return false;
    }
    // Terrain の層画像など Render が初めてロードする依存も記録する。
    m_gpuCache.Commit(ReadPreviewGpuKey(material, resources, m_resourceKey));
    m_frameCache.Commit(frameKey);
    return true;
}

bool MaterialPreviewView::RenderFrame(EditorContext& ctx, mp::RenderDesc desc)
{
    auto& resources = *ctx.resources;
    desc.time = m_animateWater ? static_cast<float>(ImGui::GetTime()) : 0.0f;
    desc.mesh = mp::ShapeMesh(resources, m_shape, desc.flavor);
    if (!desc.mesh && !mp::UsesOwnGeometry(desc.flavor)) {
        return false;
    }

    return mp::Render(*ctx.renderer, resources, desc);
}

bool MaterialPreviewView::Draw(EditorContext& ctx,
                               asset::MaterialAssetAuthoring material,
                               float previewHeight)
{
    ImGui::PushID(this);
    SynchronizeResources(ctx);

    const mp::Flavor flavor = mp::DetectFlavor(material);
    const bool gpuReady = flavor != mp::Flavor::Unsupported && EnsureGpuData(ctx, material);
    DrawToolbar(flavor, gpuReady);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float width = std::max(1.0f, available.x);
    const float requestedHeight = previewHeight > 0.0f ? previewHeight : std::max(available.y, 4.0f * ImGui::GetFrameHeight());
    const float height = std::clamp(requestedHeight, 1.0f, width);
    ImGui::InvisibleButton("##Image", { width, height });
    const ImVec2 canvasMin = ImGui::GetItemRectMin(), canvasMax = ImGui::GetItemRectMax();
    // 正方形の描画ターゲットは縦横比を保って中央に置き、左右の余白でも視点を操作できる。
    const ImVec2 imageMin{ canvasMin.x + (width - height) * 0.5f, canvasMin.y };
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        // Ctrl+Drag でライトの向きを変更する。
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
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ResetView();
    }

    if (hovered && !active && flavor != mp::Flavor::Unsupported) {
        ImGui::SetItemTooltip("%s", LOCT("Drag to orbit\nCtrl + drag to rotate light\nWheel to zoom\nDouble-click to reset view"));
    }

    DrawBackground(canvasMin, canvasMax);
    const bool rendered = gpuReady && EnsurePreviewFrame(ctx, material, flavor);
    auto& draw = *ImGui::GetWindowDrawList();
    void* rawId = rendered && ctx.imguiRenderer && ctx.resources
        ? ctx.imguiRenderer->GetImTextureID(m_renderTarget, *ctx.resources, 0)
        : nullptr;
    draw.PushClipRect(canvasMin, canvasMax, true);
    if (rawId) {
        draw.AddImage(ToImTextureID(rawId), imageMin, { imageMin.x + height, imageMin.y + height });
    } else {
        m_frameCache.Invalidate();
        DrawUnsupported(ctx, material, imageMin, height);
    }
    draw.PopClipRect();
    draw.AddRect(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_Border));

    DrawNormalMap(ctx, material);
    ImGui::PopID();
    // 描画失敗時もスウォッチを表示するため true を返す。
    return true;
}

} // namespace fbzz::editor
