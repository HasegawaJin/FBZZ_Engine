/// @file    FluidEditorExtras.cpp
/// @brief   テンプレート一覧・Baked タブ・自動保存の実装 (状態はこの翻訳単位に閉じる)
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 焼き上がりはディスクから読み直す。Atlas を引き取れるのは «id を知っている 1 人» だけ
///       (TakeBakedVolumeFlipbook は 1 度きり) なので、Volume Flipbook Baker パネルと同時に開くと
///       どちらかが空を掴む。隣に残る PNG を読めば誰が焼いても同じ絵が出る。
#include "FluidEditorExtras.hpp"

#include "FluidEditorInternal.hpp"

#include "../VolumeFlipbookComparePreview.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/FluidAssetWriters.hpp>
#include <Editor/Util/FluidRecipeWidgets.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Fluid/FluidSolver.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>

#include <DirectXTex.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fbzz::editor::fluideditor {
namespace {

/// @note 名前を長く取るのは、Editor が Unity ビルド (複数の .cpp が 1 翻訳単位へ入る) で
///       無名名前空間の名前が隣のファイルと衝突しうるため。
constexpr const char* kFluidGalleryPopupId  = "New Fluid from Template##fluid_template_gallery";
constexpr const char* kFluidSaveAsPopupId   = "Save Fluid As##fluid_save_as";
constexpr const char* kFluidTemplateFolder  = "Assets/Templates/Fluid";
/// 守るのは Fluid だけではない。Templates の下は «作り始めの原本» という約束で置いてある。
constexpr const char* kFluidTemplateRoot    = "Assets/Templates";
constexpr const char* kFluidExtension       = ".fluid";

/// サムネイルは «どんな絵が出るか» が分かれば足りる。焼きと同じ格子で解くと 1 枚で数秒かかる。
constexpr int   kFluidThumbGrid      = 40;
constexpr int   kFluidThumbImage     = 72;
constexpr int   kFluidThumbSteps     = 6;
constexpr float kFluidThumbWarmupCap = 0.4f;
constexpr int   kFluidThumbParticles = 1200;
constexpr int   kFluidGalleryMaxThumbs = 48;

constexpr float kFluidAutoSaveDelay = 3.0f;

/// WIC は呼び出しスレッドで COM が初期化されている必要がある。自分が初期化した分だけ戻す
/// (メインスレッドの STA では RPC_E_CHANGED_MODE で素通りする)。
class FluidExtrasComScope {
public:
    FluidExtrasComScope() : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~FluidExtrasComScope()
    {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
    FluidExtrasComScope(const FluidExtrasComScope&) = delete;
    FluidExtrasComScope& operator=(const FluidExtrasComScope&) = delete;

private:
    HRESULT m_result;
};

std::string FluidExtrasLowerAscii(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string FluidExtrasTrim(const std::string& text)
{
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) ++first;
    std::size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
    return text.substr(first, last - first);
}

/// @name 焼いた Atlas の読み直し

struct FluidExtrasImage {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool Valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

/// PNG / DDS を RGBA8 で読む。DDS は BC を展開する。読めなければ false。
bool FluidExtrasLoadRgba8(const std::filesystem::path& file, FluidExtrasImage& out)
{
    if (!util::FileSystem::Exists(util::FileSystem::PathToUtf8(file))) return false;

    const FluidExtrasComScope com;
    const std::wstring native = file.wstring();
    const std::string extension = FluidExtrasLowerAscii(util::FileSystem::PathToUtf8(file.extension()));
    DirectX::ScratchImage loaded;
    DirectX::TexMetadata metadata{};
    const HRESULT hr = extension == ".dds"
        ? DirectX::LoadFromDDSFile(native.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, loaded)
        : DirectX::LoadFromWICFile(native.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, loaded);
    if (FAILED(hr)) return false;

    const DirectX::Image* image = loaded.GetImage(0, 0, 0);
    if (image == nullptr) return false;
    DirectX::ScratchImage converted;
    if (DirectX::IsCompressed(image->format)) {
        if (FAILED(DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, converted))) return false;
        image = converted.GetImage(0, 0, 0);
    } else if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (FAILED(DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                    DirectX::TEX_THRESHOLD_DEFAULT, converted)))
            return false;
        image = converted.GetImage(0, 0, 0);
    }
    if (image == nullptr || image->pixels == nullptr) return false;

    out.width = static_cast<std::uint32_t>(image->width);
    out.height = static_cast<std::uint32_t>(image->height);
    const std::size_t rowBytes = static_cast<std::size_t>(out.width) * 4;
    out.rgba.resize(rowBytes * out.height);
    /// @note 行の間隔は 4·幅 とは限らない (DirectXTex はアライメントを取る)。
    for (std::uint32_t y = 0; y < out.height; ++y)
        std::memcpy(out.rgba.data() + static_cast<std::size_t>(y) * rowBytes,
                    image->pixels + static_cast<std::size_t>(y) * image->rowPitch, rowBytes);
    return true;
}

/// 候補を順に試して最初に読めたものを返す (PNG が «直せる原本»、DDS が実行時のもの)。
bool FluidExtrasLoadFirst(const std::vector<std::filesystem::path>& candidates, FluidExtrasImage& out,
                          std::string& outPath)
{
    for (const std::filesystem::path& file : candidates) {
        if (!FluidExtrasLoadRgba8(file, out)) continue;
        outPath = util::FileSystem::PathToUtf8(file);
        return true;
    }
    return false;
}

/// @name サムネイル (プリセットを数コマだけ解く)

struct FluidThumbResult {
    int index = 0;
    asset::FluidFrameImage image;
    fluid::FluidShading shading = fluid::FluidShading::Smoke;
};

struct FluidThumbChannel {
    std::atomic<bool> cancel{ false };
    std::atomic<bool> done{ false };
    std::mutex mutex;
    std::vector<FluidThumbResult> ready;

    [[nodiscard]] bool Cancelled() const { return cancel.load(std::memory_order_relaxed); }

    void Push(FluidThumbResult&& item)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ready.push_back(std::move(item));
    }

    [[nodiscard]] std::vector<FluidThumbResult> Take()
    {
        std::vector<FluidThumbResult> taken;
        std::lock_guard<std::mutex> lock(mutex);
        taken.swap(ready);
        return taken;
    }
};

struct FluidThumbRequest {
    int index = 0;
    fluid::FluidRecipe recipe;
};

/// 液体は Liquid でしか描けず、気体に Liquid を指定しても描けない (FluidPreviewCache と同じ丸め)。
fluid::FluidShading FluidExtrasEffectiveShading(const fluid::FluidRecipe& recipe)
{
    if (recipe.kind == fluid::FluidKind::Liquid) return fluid::FluidShading::Liquid;
    return recipe.render.shading == fluid::FluidShading::Liquid ? fluid::FluidShading::Smoke
                                                                : recipe.render.shading;
}

fluid::FluidRecipe FluidExtrasThumbnailRecipe(const fluid::FluidRecipe& source)
{
    fluid::FluidRecipe recipe = source;
    recipe.render.shading = FluidExtrasEffectiveShading(recipe);
    recipe.output.duration = (std::max)(recipe.output.duration, 0.05f);
    recipe.liquid.maxParticles = (std::min)(recipe.liquid.maxParticles, kFluidThumbParticles);
    return recipe;
}

void FluidExtrasSolveThumbnail(const fluid::FluidRecipe& recipe, asset::FluidFrameImage& out)
{
    /// @note 山場が来るあたり (尺の半ば) の 1 コマを出す。頭のコマはどのプリセットもほぼ空。
    const float dt = recipe.output.duration / static_cast<float>(kFluidThumbSteps * 2);
    float warmup = std::clamp(recipe.output.warmup, 0.0f, kFluidThumbWarmupCap);
    if (recipe.kind == fluid::FluidKind::Gas) {
        fluid::FluidGasSolver solver;
        solver.Reset(recipe, kFluidThumbGrid, kFluidThumbGrid, 1);
        while (warmup > 1.0e-6f) {
            const float step = (std::min)(dt, warmup);
            solver.Advance(step);
            warmup -= step;
        }
        for (int i = 0; i < kFluidThumbSteps; ++i) solver.Advance(dt);
        asset::RenderFluidGasFrame(solver, recipe, kFluidThumbImage, dt, out);
        return;
    }
    fluid::FluidLiquidSolver solver;
    solver.Reset(recipe);
    while (warmup > 1.0e-6f) {
        const float step = (std::min)(dt, warmup);
        solver.Advance(step);
        warmup -= step;
    }
    for (int i = 0; i < kFluidThumbSteps; ++i) solver.Advance(dt);
    asset::RenderFluidLiquidFrame(solver, recipe, kFluidThumbImage, dt, out);
}

void FluidExtrasThumbnailWorker(std::vector<FluidThumbRequest> requests,
                                std::shared_ptr<FluidThumbChannel> channel)
{
    /// @note Texture 形状の発生源は画像を WIC で読む。COM を初期化していないスレッドでは読めず、
    ///       «板の形に湧く» 別の絵になってしまう。
    const FluidExtrasComScope com;
    for (const FluidThumbRequest& request : requests) {
        if (channel->Cancelled()) break;
        FluidThumbResult result;
        result.index = request.index;
        result.shading = request.recipe.render.shading;
        FluidExtrasSolveThumbnail(request.recipe, result.image);
        channel->Push(std::move(result));
    }
    channel->done.store(true, std::memory_order_release);
}

/// 焼いた絵をエンジンと同じブレンドで市松の上へ重ねた不透明な色 (FluidPreviewCache と同じ式)。
void FluidExtrasComposite(const float* rgba, int x, int y, fluid::FluidShading shading, float out[3])
{
    const float background = (((x / 8) + (y / 8)) & 1) != 0 ? 0.22f : 0.12f;
    const bool premultiplied = asset::FluidShadingIsPremultiplied(shading);
    const bool additive = shading == fluid::FluidShading::Glow;
    for (int c = 0; c < 3; ++c) {
        float value = 0.0f;
        if (premultiplied)   value = rgba[c] + background * (1.0f - rgba[3]);
        else if (additive)   value = background + rgba[c] * rgba[3];
        else                 value = rgba[c] * rgba[3] + background * (1.0f - rgba[3]);
        out[c] = (std::min)(value, 1.0f);
    }
}

/// @name テンプレート一覧

enum class FluidGallerySection : std::uint8_t { Gas = 0, Liquid, User };

struct FluidGalleryEntry {
    std::string label;
    std::string description;
    /// ユーザーテンプレートの元ファイル (組み込みは空)。
    std::string sourcePath;
    fluid::FluidRecipe recipe;
    FluidGallerySection section = FluidGallerySection::Gas;
    bool valid = true;

    fluid::FluidShading shading = fluid::FluidShading::Smoke;
    asset::FluidFrameImage image;
    bool hasImage = false;
    bool uploaded = false;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    std::uint64_t resetVersion = 0;
    void* textureId = nullptr;
};

struct FluidGalleryState {
    bool openRequest = false;
    std::vector<FluidGalleryEntry> entries;
    int selected = -1;
    std::string name;
    std::string directory;
    std::string error;
    std::thread worker;
    std::shared_ptr<FluidThumbChannel> channel;

    FluidGalleryState() = default;
    FluidGalleryState(const FluidGalleryState&) = delete;
    FluidGalleryState& operator=(const FluidGalleryState&) = delete;

    /// GPU 資源には触らない (Shutdown を呼ばずに壊されても、裏のスレッドを畳むだけで済むように)。
    ~FluidGalleryState()
    {
        if (channel != nullptr) channel->cancel.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
    }
};

FluidGalleryState& FluidExtrasGallery()
{
    static FluidGalleryState state;
    return state;
}

const char* FluidExtrasPresetDescription(asset::FluidPreset preset)
{
    switch (preset) {
    case asset::FluidPreset::Smoke:       return "One puff that spreads and fades. Lifetime playback.";
    case asset::FluidPreset::Fire:        return "Fuel burns into heat and soot. Loops.";
    case asset::FluidPreset::Explosion:   return "A burst of heat and expansion, then a rolling cloud.";
    case asset::FluidPreset::Steam:       return "Thin hot vapour rising in a column. Loops.";
    case asset::FluidPreset::DustBurst:   return "Heavy ground dust pushed outwards along a floor.";
    case asset::FluidPreset::Ink:         return "Dense fluid curling in still water.";
    case asset::FluidPreset::MagicWisp:   return "Glowing haze drifting on noise. Additive, loops.";
    case asset::FluidPreset::HeatHaze:    return "Velocity as a distortion map (no colour). Loops.";
    case asset::FluidPreset::WaterSplash: return "Droplets thrown up and falling back to the floor.";
    case asset::FluidPreset::WaterJet:    return "A continuous jet of particles. Loops.";
    case asset::FluidPreset::BloodBurst:  return "Thick cohesive droplets, short and heavy.";
    case asset::FluidPreset::LavaBlob:    return "Viscous glowing liquid that slumps and sticks.";
    case asset::FluidPreset::PlasmaBurst: return "The air an arc blows apart: a blue-white ball, gone in a breath.";
    case asset::FluidPreset::ArcHaze:     return "What an arc leaves behind: thin blue haze drifting on noise. Loops.";
    case asset::FluidPreset::GroundRing:  return "A spreading dust ring for landings and ground impacts.";
    case asset::FluidPreset::ColdMist:    return "Low blue-white mist for frost, ice breaks and cold exhaust.";
    case asset::FluidPreset::ChargeVortex:return "A luminous spiral gathering into the centre before release.";
    case asset::FluidPreset::EmberBurst:  return "Five hot wisps thrown upwards, then fading.";
    case asset::FluidPreset::SigilFlare:  return "Two luminous seals igniting in sequence and fraying outwards.";
    case asset::FluidPreset::Count:
    default:                              return "";
    }
}

ImU32 FluidExtrasFallbackColor(const FluidGalleryEntry& entry)
{
    if (!entry.valid) return IM_COL32(120, 56, 48, 255);
    switch (entry.shading) {
    case fluid::FluidShading::Fire:       return IM_COL32(152, 82, 36, 255);
    case fluid::FluidShading::Glow:       return IM_COL32(92, 74, 150, 255);
    case fluid::FluidShading::Distortion: return IM_COL32(58, 108, 110, 255);
    case fluid::FluidShading::Liquid:     return IM_COL32(52, 84, 130, 255);
    case fluid::FluidShading::Smoke:
    default:                              return IM_COL32(84, 84, 90, 255);
    }
}

void FluidExtrasStopThumbnailWorker(FluidGalleryState& gallery)
{
    if (gallery.channel != nullptr) gallery.channel->cancel.store(true, std::memory_order_relaxed);
    if (gallery.worker.joinable()) gallery.worker.join();
    gallery.channel.reset();
}

void FluidExtrasReleaseThumbnails(EditorContext& ctx, FluidGalleryState& gallery)
{
    for (FluidGalleryEntry& entry : gallery.entries) {
        /// @note デバイスリセット後の古いハンドルは既に無効。返すと別の資源を消しかねない。
        if (entry.texture.IsValid() && ctx.resources != nullptr
            && entry.resetVersion == ctx.resources->GetResetVersion())
            ctx.resources->Release(entry.texture);
        entry.texture = {};
        entry.textureId = nullptr;
        entry.uploaded = false;
    }
}

void FluidExtrasCloseGallery(EditorContext& ctx, FluidGalleryState& gallery)
{
    FluidExtrasStopThumbnailWorker(gallery);
    FluidExtrasReleaseThumbnails(ctx, gallery);
    gallery.entries.clear();
    gallery.selected = -1;
}

void FluidExtrasBuildEntries(EditorContext& ctx, FluidGalleryState& gallery)
{
    FluidExtrasCloseGallery(ctx, gallery);

    for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i) {
        const auto preset = static_cast<asset::FluidPreset>(i);
        FluidGalleryEntry entry;
        entry.label = asset::FluidPresetName(preset);
        entry.description = FluidExtrasPresetDescription(preset);
        entry.recipe = asset::MakeFluidPreset(preset);
        entry.shading = FluidExtrasEffectiveShading(entry.recipe);
        entry.section = entry.recipe.kind == fluid::FluidKind::Liquid ? FluidGallerySection::Liquid
                                                                     : FluidGallerySection::Gas;
        gallery.entries.push_back(std::move(entry));
    }

    if (!ctx.projectRoot.empty()) {
        const std::filesystem::path folder =
            util::FileSystem::PathFromUtf8(ctx.projectRoot) / kFluidTemplateFolder;
        if (util::FileSystem::IsDirectory(util::FileSystem::PathToUtf8(folder))) {
            for (const std::filesystem::path& file : util::FileSystem::ListFilesRecursive(folder)) {
                if (FluidExtrasLowerAscii(util::FileSystem::PathToUtf8(file.extension())) != kFluidExtension)
                    continue;
                FluidGalleryEntry entry;
                entry.section = FluidGallerySection::User;
                entry.label = util::FileSystem::PathToUtf8(file.stem());
                entry.sourcePath = util::FileSystem::PathToUtf8(file);
                std::string error;
                entry.valid = asset::LoadFluidRecipe(entry.sourcePath, entry.recipe, &error);
                entry.description = entry.valid ? NormalizeAssetPath(entry.sourcePath)
                                                : "Cannot read this template: " + error;
                entry.shading = FluidExtrasEffectiveShading(entry.recipe);
                gallery.entries.push_back(std::move(entry));
            }
        }
    }

    std::vector<FluidThumbRequest> requests;
    for (int i = 0; i < static_cast<int>(gallery.entries.size()); ++i) {
        if (static_cast<int>(requests.size()) >= kFluidGalleryMaxThumbs) break;
        const FluidGalleryEntry& entry = gallery.entries[static_cast<std::size_t>(i)];
        if (!entry.valid) continue;
        FluidThumbRequest request;
        request.index = i;
        request.recipe = FluidExtrasThumbnailRecipe(entry.recipe);
        requests.push_back(std::move(request));
    }
    if (requests.empty()) return;
    gallery.channel = std::make_shared<FluidThumbChannel>();
    gallery.worker = std::thread([requests = std::move(requests), shared = gallery.channel]() mutable {
        FluidExtrasThumbnailWorker(std::move(requests), shared);
    });
}

/// 解けた絵を取り込み、まだ載せていないものを GPU へ上げる。
void FluidExtrasTickThumbnails(EditorContext& ctx, FluidGalleryState& gallery)
{
    if (gallery.channel != nullptr) {
        for (FluidThumbResult& item : gallery.channel->Take()) {
            if (item.index < 0 || item.index >= static_cast<int>(gallery.entries.size())) continue;
            FluidGalleryEntry& entry = gallery.entries[static_cast<std::size_t>(item.index)];
            entry.image = std::move(item.image);
            entry.shading = item.shading;
            entry.hasImage = entry.image.size > 0;
            entry.uploaded = false;
        }
        if (gallery.channel->done.load(std::memory_order_acquire)) {
            if (gallery.worker.joinable()) gallery.worker.join();
            gallery.channel.reset();
        }
    }
    if (ctx.resources == nullptr || ctx.imguiRenderer == nullptr) return;

    const std::uint64_t resetVersion = ctx.resources->GetResetVersion();
    std::vector<std::uint8_t> pixels;
    for (FluidGalleryEntry& entry : gallery.entries) {
        if (entry.resetVersion != resetVersion) {
            /// @note デバイスリセットで古いハンドルは無効。載せ直せないなら色の四角へ戻す。
            entry.texture = {};
            entry.textureId = nullptr;
            entry.uploaded = false;
        }
        if (!entry.hasImage || entry.uploaded) continue;
        const int size = entry.image.size;
        const std::size_t pixelCount = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
        if (size <= 0 || entry.image.rgba.size() < pixelCount * 4) continue;
        if (!entry.texture.IsValid() || entry.resetVersion != resetVersion) {
            const auto side = static_cast<std::uint32_t>(size);
            entry.texture = ctx.resources->CreateDynamicTexture(side, side,
                                                                renderer::DynamicTextureFormat::RGBA8);
            entry.resetVersion = resetVersion;
            if (!entry.texture.IsValid()) {
                /// @note 未対応バックエンド。以降は作り直さず、呼び手が色の四角を描く。
                entry.hasImage = false;
                continue;
            }
            entry.textureId = ctx.imguiRenderer->GetImTextureID(entry.texture, *ctx.resources);
        }
        renderer::ITexture* gpu = ctx.resources->Get(entry.texture);
        if (gpu == nullptr || entry.textureId == nullptr) continue;
        pixels.resize(pixelCount * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(size)
                                        + static_cast<std::size_t>(x);
                float color[3];
                FluidExtrasComposite(&entry.image.rgba[pixel * 4], x, y, entry.shading, color);
                for (int c = 0; c < 3; ++c)
                    pixels[pixel * 4 + static_cast<std::size_t>(c)] =
                        static_cast<std::uint8_t>(color[c] * 255.0f + 0.5f);
                pixels[pixel * 4 + 3] = 255;
            }
        }
        const auto side = static_cast<std::uint32_t>(size);
        if (!gpu->UpdateRegion(0, 0, side, side, pixels.data(), side * 4)) continue;
        entry.uploaded = true;
        /// @note 絵はもう GPU にある。float のコピーを抱え続けない。
        std::vector<float>().swap(entry.image.rgba);
        std::vector<float>().swap(entry.image.motion);
    }
}

/// 入力された名前をファイル名として使えるか。使えれば ".fluid" 付きの名前を返す。
bool FluidExtrasValidateName(const std::string& input, std::string& outFile, std::string& outError)
{
    const std::string name = FluidExtrasTrim(input);
    if (name.empty()) {
        outError = "Enter a name for the new .fluid.";
        return false;
    }
    for (const char c : name) {
        const bool reserved = c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"'
                           || c == '<' || c == '>' || c == '|';
        if (reserved || static_cast<unsigned char>(c) < 0x20) {
            outError = "The name cannot contain \\ / : * ? \" < > |";
            return false;
        }
    }
    if (name.back() == '.') {
        outError = "The name cannot end with a dot.";
        return false;
    }
    const std::string lower = FluidExtrasLowerAscii(name);
    const bool hasExtension = lower.size() > 6
        && lower.compare(lower.size() - 6, 6, kFluidExtension) == 0;
    outFile = hasExtension ? name : name + kFluidExtension;
    return true;
}

/// 名前とフォルダを検めてレシピを新しい .fluid へ書く。書けたら実パス、駄目なら空 (outError に理由)。
std::string FluidExtrasWriteNewRecipe(EditorContext& ctx, const fluid::FluidRecipe& recipe,
                                      const std::string& rawName, const std::string& rawDirectory,
                                      std::string& outError)
{
    std::string file;
    if (!FluidExtrasValidateName(rawName, file, outError)) return {};

    const std::string directoryText = FluidExtrasTrim(rawDirectory);
    if (directoryText.empty()) {
        outError = "Enter the folder to create the .fluid in.";
        return {};
    }
    const std::filesystem::path directory = util::FileSystem::PathFromUtf8(directoryText);
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        outError = "Cannot create the folder: " + directoryText;
        return {};
    }
    const std::filesystem::path target = directory / util::FileSystem::PathFromUtf8(file);
    const std::string absPath = util::FileSystem::PathToUtf8(target);
    if (util::FileSystem::Exists(absPath)) {
        /// @note 上書きは «作る» 操作の顔をして人の作業を消す。名前を変えてもらう。
        outError = "A file with that name already exists.";
        return {};
    }
    if (!asset::SaveFluidRecipe(absPath, recipe)) {
        outError = "Cannot write the .fluid: " + absPath;
        return {};
    }
    ctx.requestAssetBrowserRefresh = true;
    outError.clear();
    return absPath;
}

/// そのまま確定できる名前 (同じフォルダに無くなるまで連番を足す)。
std::string FluidExtrasUniqueName(const std::string& stem, const std::string& directory)
{
    const std::filesystem::path folder = util::FileSystem::PathFromUtf8(FluidExtrasTrim(directory));
    const std::string base = stem.empty() ? std::string("NewFluid") : stem;
    std::string candidate = base;
    for (int suffix = 1; suffix < 1000; ++suffix) {
        const std::filesystem::path target =
            folder / util::FileSystem::PathFromUtf8(candidate + kFluidExtension);
        if (!util::FileSystem::Exists(util::FileSystem::PathToUtf8(target))) break;
        candidate = base + " " + std::to_string(suffix);
    }
    return candidate;
}

std::string FluidExtrasCreateFromEntry(EditorContext& ctx, FluidGalleryState& gallery)
{
    if (gallery.selected < 0 || gallery.selected >= static_cast<int>(gallery.entries.size())) {
        gallery.error = "Pick a template first.";
        return {};
    }
    const FluidGalleryEntry& entry = gallery.entries[static_cast<std::size_t>(gallery.selected)];
    if (!entry.valid) {
        gallery.error = "This template cannot be read.";
        return {};
    }
    return FluidExtrasWriteNewRecipe(ctx, entry.recipe, gallery.name, gallery.directory, gallery.error);
}

const char* FluidExtrasSectionTitle(FluidGallerySection section)
{
    switch (section) {
    case FluidGallerySection::Gas:    return "Gas presets";
    case FluidGallerySection::Liquid: return "Liquid presets";
    case FluidGallerySection::User:
    default:                          return "Project templates (Assets/Templates/Fluid)";
    }
}

/// 一覧の中身。ダブルクリックで作りたいときは outCreateNow を立てる。
void FluidExtrasDrawEntries(FluidGalleryState& gallery, bool& outCreateNow)
{
    const float thumb = static_cast<float>(kFluidThumbImage);
    const float pad = 6.0f;
    const float cellWidth = thumb + pad * 2.0f;
    const float cellHeight = thumb + pad * 2.0f + ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float available = (std::max)(ImGui::GetContentRegionAvail().x, cellWidth);
    const int columns = (std::max)(1, static_cast<int>(available / (cellWidth + spacing)));

    for (int section = 0; section <= static_cast<int>(FluidGallerySection::User); ++section) {
        bool header = false;
        int column = 0;
        for (int i = 0; i < static_cast<int>(gallery.entries.size()); ++i) {
            FluidGalleryEntry& entry = gallery.entries[static_cast<std::size_t>(i)];
            if (static_cast<int>(entry.section) != section) continue;
            if (!header) {
                ImGui::SeparatorText(FluidExtrasSectionTitle(entry.section));
                header = true;
                column = 0;
            }
            if (column > 0) ImGui::SameLine();

            ImGui::PushID(i);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const bool selected = gallery.selected == i;
            if (ImGui::Selectable("##cell", selected, ImGuiSelectableFlags_AllowDoubleClick,
                                  { cellWidth, cellHeight })) {
                gallery.selected = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) outCreateNow = true;
            }
            if (ImGui::IsItemHovered() && !entry.description.empty())
                ImGui::SetTooltip("%s", entry.description.c_str());

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 thumbMin{ origin.x + pad, origin.y + pad };
            const ImVec2 thumbMax{ thumbMin.x + thumb, thumbMin.y + thumb };
            if (entry.uploaded && entry.textureId != nullptr)
                drawList->AddImage(widgets::ToImTextureID(entry.textureId), thumbMin, thumbMax);
            else
                drawList->AddRectFilled(thumbMin, thumbMax, FluidExtrasFallbackColor(entry));
            drawList->AddRect(thumbMin, thumbMax,
                              selected ? IM_COL32(236, 180, 90, 255) : IM_COL32(0, 0, 0, 160));
            const std::string label = widgets::ElideToWidth(entry.label.c_str(), cellWidth - pad * 2.0f);
            drawList->AddText({ origin.x + pad, thumbMax.y + 2.0f }, IM_COL32(228, 228, 232, 255),
                              label.c_str());
            ImGui::PopID();

            ++column;
            if (column >= columns) column = 0;
        }
    }
    if (gallery.entries.empty()) ImGui::TextDisabled("No templates.");
}

/// @name Baked タブ

struct FluidBakedState {
    bool has = false;
    std::string fluidPath;
    std::string colorPath;
    std::string motionPath;
    std::string materialPath;
    /// 3D (Volume Flipbook Baker) の出力を見ているか。
    bool volume = false;
    bool pendingLoad = false;
    bool loaded = false;
    bool hasMotion = false;
    std::string message;
    bool messageIsError = false;

    fluid::FluidRecipe recipe;
    fluid::FluidShading shading = fluid::FluidShading::Smoke;

    VolumeFlipbookComparePreview compare;
    bool playing = true;
    float time = 0.0f;
    float fps = 24.0f;
    float strengthScale = 1.0f;
    asset::VolumePreviewBackground background = asset::VolumePreviewBackground::Dark;
};

FluidBakedState& FluidExtrasBaked()
{
    static FluidBakedState state;
    return state;
}

/// `<stem>` から焼き上がりの候補を組む。PNG を先に置く (BC を展開せずに読める)。
std::vector<std::filesystem::path> FluidExtrasOutputCandidates(const std::filesystem::path& base,
                                                               bool volume, bool motion)
{
    const std::string stem = util::FileSystem::PathToUtf8(base);
    if (volume) {
        if (motion) return { util::FileSystem::PathFromUtf8(stem + "_mv.png"),
                             util::FileSystem::PathFromUtf8(stem + "_mv.dds") };
        return { util::FileSystem::PathFromUtf8(stem + ".png"),
                 util::FileSystem::PathFromUtf8(stem + ".dds") };
    }
    if (motion) return { util::FileSystem::PathFromUtf8(stem + "_MV.png"),
                         util::FileSystem::PathFromUtf8(stem + "_MV.dds") };
    return { util::FileSystem::PathFromUtf8(stem + "_Flipbook.png"),
             util::FileSystem::PathFromUtf8(stem + "_Flipbook.dds") };
}

/// 焼いたときのコマ割り・再生速度・MV の強さ。隣の .mat が «焼いた結果そのもの» を持っているので
/// まずそれを読み、無ければレシピの [output] から組む。
void FluidExtrasResolvePlayback(const FluidBakedState& baked, asset::FlipbookGrid& outGrid, float& outFps,
                                bool& outLoop, float& outStrength)
{
    const int columns = (std::max)(baked.recipe.output.columns, 1);
    const int rows = (std::max)(baked.recipe.output.rows, 1);
    outGrid = { columns, rows, columns * rows };
    outLoop = baked.recipe.output.loop;
    outFps = static_cast<float>(outGrid.frameCount)
           / (std::max)(baked.recipe.output.duration, 1.0e-3f);
    outStrength = 0.0f;

    asset::MaterialAsset material;
    if (baked.materialPath.empty() || !asset::LoadMaterialAssetFromFile(baked.materialPath, material)) return;
    const asset::ParticleFlipbookSettings& flipbook = material.particle.flipbook;
    if (flipbook.FrameCount() > 1) {
        outGrid = { (std::max)(flipbook.spriteColumns, 1), (std::max)(flipbook.spriteRows, 1),
                    flipbook.FrameCount() };
        outFps = static_cast<float>(outGrid.frameCount)
               / (std::max)(baked.recipe.output.duration, 1.0e-3f);
    }
    if (flipbook.flipbookMode == scene::ParticleFlipbookMode::FramesPerSecond) {
        outFps = (std::max)(flipbook.flipbookFramesPerSecond, 0.1f);
        outLoop = true;
    }
    if (flipbook.motionVectorFlipbook) outStrength = flipbook.motionVectorStrength;
}

/// 2D の Smoke / Liquid はストレートアルファで焼いてある。比較プレビューは事前乗算で合成するので、
/// 読んだ値のまま渡すと縁が濃くなる。シェーダーがリニアへ直してから掛けるぶんを見越して乗せる。
void FluidExtrasPremultiply(std::vector<std::uint8_t>& rgba)
{
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const float alpha = static_cast<float>(rgba[i + 3]) / 255.0f;
        const float scale = std::pow(alpha, 1.0f / 2.2f);
        for (std::size_t c = 0; c < 3; ++c)
            rgba[i + c] = static_cast<std::uint8_t>(static_cast<float>(rgba[i + c]) * scale + 0.5f);
    }
}

void FluidExtrasLoadBaked(EditorContext& ctx, FluidBakedState& baked)
{
    baked.pendingLoad = false;
    baked.loaded = false;
    if (ctx.resources == nullptr) {
        baked.message = "No renderer: the baked atlas cannot be shown.";
        baked.messageIsError = true;
        return;
    }

    FluidExtrasImage color;
    if (!FluidExtrasLoadFirst(FluidExtrasOutputCandidates(
                                  util::FileSystem::PathFromUtf8(baked.fluidPath).replace_extension(),
                                  baked.volume, false),
                              color, baked.colorPath)
        || !color.Valid()) {
        baked.message = "The baked atlas is not next to the .fluid yet.";
        baked.messageIsError = true;
        return;
    }
    FluidExtrasImage motion;
    baked.hasMotion = FluidExtrasLoadFirst(
        FluidExtrasOutputCandidates(util::FileSystem::PathFromUtf8(baked.fluidPath).replace_extension(),
                                    baked.volume, true),
        motion, baked.motionPath)
        && motion.width == color.width && motion.height == color.height;
    if (!baked.hasMotion) baked.motionPath.clear();

    asset::FlipbookGrid grid;
    float fps = 24.0f;
    bool loop = false;
    float strength = 0.0f;
    FluidExtrasResolvePlayback(baked, grid, fps, loop, strength);

    asset::BakedVolumeFlipbook flipbook;
    flipbook.atlasWidth = color.width;
    flipbook.atlasHeight = color.height;
    flipbook.tileSize = color.width / static_cast<std::uint32_t>((std::max)(grid.columns, 1));
    flipbook.grid = grid;
    flipbook.frameDt = 1.0f / (std::max)(fps, 0.1f);
    flipbook.loop = loop;
    flipbook.motionStrength = baked.hasMotion ? strength : 0.0f;
    /// @note 2D は shading で事前乗算かどうかが決まる。3D は必ず事前乗算で焼いてある。
    if (!baked.volume && !asset::FluidShadingIsPremultiplied(baked.shading))
        FluidExtrasPremultiply(color.rgba);
    flipbook.colorRgba8 = std::move(color.rgba);
    if (baked.hasMotion) {
        flipbook.motionRgba8 = std::move(motion.rgba);
    } else {
        /// @note MV を焼いていないレシピでも «焼いた結果» は見せたい。強さ 0 なら warp は効かないので、
        ///       動かない MV (0.5 中心) を渡して左右を同じ絵にする。
        flipbook.motionRgba8.assign(static_cast<std::size_t>(color.width) * color.height * 4, 0);
        for (std::size_t i = 0; i + 3 < flipbook.motionRgba8.size(); i += 4) {
            flipbook.motionRgba8[i] = 128;
            flipbook.motionRgba8[i + 1] = 128;
            flipbook.motionRgba8[i + 3] = 255;
        }
    }

    std::string error;
    if (!baked.compare.Upload(*ctx.resources, std::move(flipbook), error)) {
        baked.message = error;
        baked.messageIsError = true;
        return;
    }
    baked.loaded = true;
    baked.time = 0.0f;
    baked.playing = true;
    baked.fps = fps;
    baked.message.clear();
    baked.messageIsError = false;
}

void FluidExtrasDrawOutputRow(const char* label, const std::string& absPath)
{
    if (absPath.empty()) return;
    const std::string shown = NormalizeAssetPath(absPath);
    ImGui::PushID(label);
    if (ImGui::SmallButton("Reveal")) widgets::RequestAssetReveal(shown, false);
    ImGui::SameLine();
    ImGui::TextDisabled("%s: %s", label, shown.c_str());
    ImGui::PopID();
}

/// @name 名前を付けて保存

struct FluidSaveAsState {
    bool openRequest = false;
    bool active = false;
    std::string name;
    std::string directory;
    /// 元の .fluid (人に見せる形)。
    std::string sourceLabel;
    std::string error;
};

FluidSaveAsState& FluidExtrasSaveAs()
{
    static FluidSaveAsState state;
    return state;
}

/// @name 自動保存

struct FluidAutoSaveState {
    bool enabled = false;
    bool hasRevision = false;
    std::uint64_t revision = 0;
    /// 最後に Revision が動いてからの秒。
    float idle = 0.0f;
};

FluidAutoSaveState& FluidExtrasAutoSave()
{
    static FluidAutoSaveState state;
    return state;
}

/// @name 部品の控え

/// @note 部品だけの型は作らず «レシピ 1 つ» に入れて持つ。3 種類の部品型を union / variant で
///       名指しすると型が増えるたび触る場所が増えるが、レシピに 1 つ抱えればコピー / ペーストは
///       既存 vector の値コピーで済み、型を知る場所は switch 3 本に収まる。
struct FluidPartClipboardState {
    FluidSelectionKind kind = FluidSelectionKind::None;
    fluid::FluidRecipe holder;
    std::string label;
};

FluidPartClipboardState& FluidExtrasPartClipboard()
{
    static FluidPartClipboardState clipboard;
    return clipboard;
}

/// list が指す vector 1 本だけを見る (3 種類に同じ処理を書かないため)。
/// 別のリストへ渡すのには使えない — 呼び手が switch で型を突き合わせる必要がある。
template <typename Fn>
bool FluidExtrasVisitPartVector(const fluid::FluidRecipe& recipe, FluidSelectionKind list, Fn&& fn)
{
    switch (list) {
    case FluidSelectionKind::Source:   fn(recipe.sources); return true;
    case FluidSelectionKind::Force:    fn(recipe.forces); return true;
    case FluidSelectionKind::Collider: fn(recipe.colliders); return true;
    default:                           return false;
    }
}

/// "Vortex (2)" → "Vortex"。連番を足すたびに "(1) (1)" と伸びるのを防ぐ。
std::string FluidExtrasStripNameCounter(const std::string& name)
{
    if (name.size() < 4 || name.back() != ')') return name;
    const std::size_t open = name.rfind(" (");
    if (open == std::string::npos || open + 2 >= name.size() - 1) return name;
    for (std::size_t i = open + 2; i + 1 < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return name;
    return name.substr(0, open);
}

} // namespace

/// @name テンプレートの置き場

bool IsFluidTemplatePath(const EditorContext& ctx, const std::string& absPath)
{
    if (absPath.empty() || ctx.projectRoot.empty()) return false;
    const std::string root =
        util::FileSystem::NormalizePathSeparators(ctx.projectRoot) + "/" + kFluidTemplateRoot;
    return util::FileSystem::IsChildPathText(absPath, root);
}

/// @name 名前を付けて保存

void OpenFluidSaveAsModal(const std::string& sourcePath, const std::string& defaultDirectory)
{
    FluidSaveAsState& saveAs = FluidExtrasSaveAs();
    saveAs.directory = defaultDirectory;
    saveAs.name = FluidExtrasUniqueName(
        util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(sourcePath).stem()), saveAs.directory);
    saveAs.sourceLabel = NormalizeAssetPath(sourcePath);
    saveAs.error.clear();
    /// @note 開くのは次に描くとき。呼び手は ImGui の ID スタックがどこにあるか分からない所 (ModalDialog の
    ///       コールバックや終了時の一括保存) からも来る。
    saveAs.openRequest = true;
}

FluidSaveAsResult DrawFluidSaveAsModal(EditorContext& ctx, FluidDocument& document)
{
    FluidSaveAsState& saveAs = FluidExtrasSaveAs();
    if (saveAs.openRequest) {
        saveAs.openRequest = false;
        saveAs.active = true;
        ImGui::OpenPopup(kFluidSaveAsPopupId);
    }
    if (!saveAs.active) return {};
    if (!document.IsOpen()) {
        saveAs.active = false;
        return {};
    }

    ImGui::SetNextWindowSize({ 560.0f, 0.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kFluidSaveAsPopupId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        saveAs.active = false;
        return {};
    }

    ImGui::TextWrapped("%s is a project template. Saving over it would replace the original, so this writes"
                       " the recipe you are editing to a new .fluid.",
                       saveAs.sourceLabel.c_str());
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-120.0f);
    widgets::InputString("Name##fe_saveas_name", saveAs.name, 128);
    ImGui::SetNextItemWidth(-120.0f);
    widgets::InputString("Folder##fe_saveas_folder", saveAs.directory, 512);
    if (!saveAs.error.empty())
        ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f }, "%s", saveAs.error.c_str());

    FluidSaveAsResult result;
    if (ImGui::Button("Save As##fe_saveas_save", { 140.0f, 0.0f })) {
        const std::string written =
            FluidExtrasWriteNewRecipe(ctx, document.Recipe(), saveAs.name, saveAs.directory, saveAs.error);
        if (!written.empty()) {
            result.action = FluidSaveAsAction::SavedCopy;
            result.path = written;
            saveAs.active = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##fe_saveas_cancel", { 140.0f, 0.0f })) {
        saveAs.active = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Meant to change the template itself?");
    if (ImGui::Button("Overwrite this template##fe_saveas_overwrite", { 240.0f, 0.0f })) {
        result.action = FluidSaveAsAction::Overwrite;
        saveAs.active = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip("Writes over %s. Auto Save still leaves templates alone.",
                          saveAs.sourceLabel.c_str());
    ImGui::EndPopup();
    return result;
}

/// @name テンプレート一覧

void OpenFluidTemplateGallery()
{
    FluidExtrasGallery().openRequest = true;
}

std::string DrawFluidTemplateGallery(EditorContext& ctx, const std::string& defaultDirectory)
{
    FluidGalleryState& gallery = FluidExtrasGallery();
    if (gallery.openRequest) {
        gallery.openRequest = false;
        FluidExtrasBuildEntries(ctx, gallery);
        gallery.selected = gallery.entries.empty() ? -1 : 0;
        gallery.name = "NewFluid";
        gallery.directory = !defaultDirectory.empty() ? defaultDirectory
                          : ctx.projectRoot.empty()   ? std::string{}
                                                      : ctx.projectRoot + "/Assets";
        gallery.error.clear();
        ImGui::OpenPopup(kFluidGalleryPopupId);
    }
    if (gallery.entries.empty()) return {};
    FluidExtrasTickThumbnails(ctx, gallery);

    ImGui::SetNextWindowSize({ 700.0f, 560.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kFluidGalleryPopupId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        /// @note 閉じている間は裏の解きもサムネイルも抱えない (次に開いたときに作り直す)。
        FluidExtrasCloseGallery(ctx, gallery);
        return {};
    }

    bool create = false;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 4.0f + ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##fluid_template_entries", { 0.0f, -footer }, ImGuiChildFlags_Borders))
        FluidExtrasDrawEntries(gallery, create);
    ImGui::EndChild();

    if (gallery.selected >= 0 && gallery.selected < static_cast<int>(gallery.entries.size())) {
        const FluidGalleryEntry& entry = gallery.entries[static_cast<std::size_t>(gallery.selected)];
        ImGui::TextDisabled("%s — %s", entry.label.c_str(), entry.description.c_str());
    } else {
        ImGui::TextDisabled("Pick a starting point.");
    }
    widgets::InputString("Name", gallery.name, 128);
    widgets::InputString("Folder", gallery.directory, 512);
    if (!gallery.error.empty()) ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f }, "%s", gallery.error.c_str());

    if (ImGui::Button("Create", { 120.0f, 0.0f })) create = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120.0f, 0.0f })) ImGui::CloseCurrentPopup();

    std::string created;
    if (create) {
        created = FluidExtrasCreateFromEntry(ctx, gallery);
        if (!created.empty()) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return created;
}

/// @name Baked タブ

void SetFluidBakedResult(EditorContext& ctx, const std::string& fluidPath)
{
    FluidBakedState& baked = FluidExtrasBaked();
    if (ctx.resources != nullptr) baked.compare.Release(*ctx.resources);
    baked.loaded = false;
    baked.hasMotion = false;
    baked.colorPath.clear();
    baked.motionPath.clear();
    baked.message.clear();
    baked.messageIsError = false;
    baked.fluidPath = fluidPath;
    baked.has = !fluidPath.empty();
    if (!baked.has) return;

    baked.recipe = {};
    if (!asset::LoadFluidRecipe(fluidPath, baked.recipe)) {
        baked.message = "Cannot read the .fluid: " + NormalizeAssetPath(fluidPath);
        baked.messageIsError = true;
        return;
    }
    baked.volume = baked.recipe.bake.mode == fluid::FluidBakeMode::Volume3D;
    baked.shading = FluidExtrasEffectiveShading(baked.recipe);
    baked.materialPath = SiblingMaterialPath(fluidPath);
    if (!util::FileSystem::Exists(baked.materialPath)) baked.materialPath.clear();
    /// @note 読むのはタブを開いたとき。Atlas は数十 MB あり、焼き終わったフレームで読むと画面が飛ぶ。
    baked.pendingLoad = true;
}

bool HasFluidBakedResult()
{
    return FluidExtrasBaked().has;
}

void DrawFluidBakedTab(EditorContext& ctx, State& state)
{
    FluidBakedState& baked = FluidExtrasBaked();
    if (!baked.has) {
        ImGui::TextDisabled("Bake this recipe to compare the result here.");
        ImGui::TextDisabled("The bake writes the atlas next to the .fluid; this tab reads it back.");
        return;
    }
    if (baked.pendingLoad) {
        FluidExtrasLoadBaked(ctx, baked);
        if (baked.messageIsError && !baked.message.empty()) SetStatus(state, baked.message, true);
    }

    ImGui::TextDisabled("%s", NormalizeAssetPath(baked.fluidPath).c_str());
    FluidExtrasDrawOutputRow("Atlas", baked.colorPath);
    FluidExtrasDrawOutputRow("Motion", baked.motionPath);
    FluidExtrasDrawOutputRow("Material", baked.materialPath);

    if (!baked.loaded) {
        if (!baked.message.empty()) {
            if (baked.messageIsError) ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f }, "%s", baked.message.c_str());
            else                      ImGui::TextWrapped("%s", baked.message.c_str());
        }
        if (ImGui::Button("Reload", { 120.0f, 0.0f })) baked.pendingLoad = true;
        return;
    }

    if (ImGui::Button(baked.playing ? "Pause" : "Play", { 64.0f, 0.0f })) baked.playing = !baked.playing;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat("Playback FPS", &baked.fps, 0.1f, 1.0f, 60.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Baked FPS")) baked.fps = 1.0f / (std::max)(baked.compare.FrameDt(), 1.0e-4f);

    const int frames = (std::max)(baked.compare.FrameCount(), 1);
    float framePosition = baked.time * baked.fps;
    if (ImGui::SliderFloat("Frame", &framePosition, 0.0f, static_cast<float>((std::max)(frames - 1, 1)),
                           "%.2f")) {
        baked.playing = false;
        baked.time = framePosition / (std::max)(baked.fps, 0.1f);
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("MV Strength", &baked.strengthScale, 0.0f, 2.0f, "x%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scales the baked S. 1 is the value the atlas was made for.");
    ImGui::SameLine();
    static constexpr const char* kFluidBakedBackgrounds[] = { "Dark", "Light", "Checker" };
    int background = static_cast<int>(baked.background);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Background", &background, kFluidBakedBackgrounds, IM_ARRAYSIZE(kFluidBakedBackgrounds)))
        baked.background = static_cast<asset::VolumePreviewBackground>(background);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) baked.pendingLoad = true;

    if (baked.playing) {
        const float fps = (std::max)(baked.fps, 0.1f);
        const float cycle = static_cast<float>(frames) / fps + (baked.compare.Loops() ? 0.0f : 0.5f);
        baked.time = std::fmod(baked.time + ImGui::GetIO().DeltaTime, (std::max)(cycle, 1.0e-3f));
    }
    if (ctx.renderer == nullptr || ctx.resources == nullptr || ctx.imguiRenderer == nullptr) return;
    /// @note 比較の絵はこのフレームの中で描く。ImGui が実際に描くのはフレームの終わりなので順序は保たれる。
    baked.compare.Render(*ctx.renderer, *ctx.resources, baked.time, baked.fps, baked.strengthScale,
                         baked.background);
    void* rawId = ctx.imguiRenderer->GetImTextureID(baked.compare.Target(), *ctx.resources, 0);
    if (rawId == nullptr) return;

    const float width = (std::max)(ImGui::GetContentRegionAvail().x, 128.0f);
    ImGui::Image(widgets::ToImTextureID(rawId), { width, width * 0.5f });
    const ImVec2 imageMin = ImGui::GetItemRectMin();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText({ imageMin.x + 6.0f, imageMin.y + 4.0f }, IM_COL32(235, 235, 235, 255), "No MV");
    drawList->AddText({ imageMin.x + width * 0.5f + 6.0f, imageMin.y + 4.0f }, IM_COL32(235, 235, 235, 255),
                      "With MV");
    drawList->AddLine({ imageMin.x + width * 0.5f, imageMin.y },
                      { imageMin.x + width * 0.5f, imageMin.y + width * 0.5f }, IM_COL32(20, 20, 20, 255),
                      2.0f);

    ImGui::TextDisabled("Frame %d  blend %.2f   S = %.4f%s", baked.compare.CurrentFrame(),
                        baked.compare.CurrentBlend(), baked.compare.MotionStrength(),
                        baked.compare.Loops() ? "   (Loop)" : "");
    if (!baked.hasMotion)
        ImGui::TextDisabled("No motion vectors were baked, so both halves are the same.");
    if (baked.shading == fluid::FluidShading::Glow)
        ImGui::TextDisabled("Additive shading is composited as alpha here; in game it adds to the scene.");
}

void ShutdownFluidBakedTab(EditorContext& ctx)
{
    FluidBakedState& baked = FluidExtrasBaked();
    if (ctx.resources != nullptr) baked.compare.Release(*ctx.resources);
    baked.loaded = false;
    baked.pendingLoad = baked.has;
    /// @note 一覧のサムネイルもここで返す。GPU 資源を持つのはこの 2 つだけで、返す口は他に無い。
    FluidExtrasCloseGallery(ctx, FluidExtrasGallery());
}

/// @name 自動保存

bool TickFluidAutoSave(EditorContext& ctx, FluidDocument& document)
{
    FluidAutoSaveState& autoSave = FluidExtrasAutoSave();
    if (!document.IsOpen()) {
        autoSave.hasRevision = false;
        autoSave.idle = 0.0f;
        return false;
    }
    /// @note テンプレートは «作り始めの原本» で、開いて値を見ただけの人も多い。3 秒の無操作で黙って書くと、
    ///       覗いただけのつもりが原本ごと別物になる (ShockRing.fluid が実際に消えた)。上書きは人が選ぶ。
    if (IsFluidTemplatePath(ctx, document.Path())) return false;
    const std::uint64_t revision = document.Revision();
    if (!autoSave.hasRevision || revision != autoSave.revision) {
        autoSave.hasRevision = true;
        autoSave.revision = revision;
        autoSave.idle = 0.0f;
        return false;
    }
    autoSave.idle += ImGui::GetIO().DeltaTime;
    if (!autoSave.enabled || !document.IsDirty()) return false;
    /// @note ドラッグの途中で書くと、手を離すまでの中間の姿が .fluid に残る (Undo も 1 つに畳めていない)。
    if (document.InInteractiveEdit()) return false;
    /// @note 外で書き換わったファイルへ黙って上書きすると、相手の変更が消える。解決は人が選ぶ。
    if (document.HasExternalConflict()) return false;
    if (autoSave.idle < kFluidAutoSaveDelay) return false;

    std::string error;
    const bool saved = document.Save(ctx, error);
    /// @note 失敗しても毎フレーム書きに行かないよう、成否に関わらず間を置き直す。
    autoSave.idle = 0.0f;
    autoSave.revision = document.Revision();
    return saved;
}

bool FluidAutoSaveEnabled(const EditorContext& ctx)
{
    (void)ctx;
    return FluidExtrasAutoSave().enabled;
}

void SetFluidAutoSaveEnabled(EditorContext& ctx, bool enabled)
{
    FluidAutoSaveState& autoSave = FluidExtrasAutoSave();
    if (autoSave.enabled == enabled) return;
    autoSave.enabled = enabled;
    autoSave.idle = 0.0f;
    /// @note 次の起動へ持ち越す値なので、終了を待たずに editor_settings.toml へ書かせる
    ///       (EditorSettings::fluidEditorAutoSave が保存先)。
    ctx.requestEditorSettingsSave = true;
}

/// @name 部品の控え

void SetFluidPartClipboard(const fluid::FluidRecipe& recipe, FluidSelectionKind list, int index)
{
    if (index < 0) return;
    const auto at = static_cast<std::size_t>(index);
    FluidPartClipboardState next;
    switch (list) {
    case FluidSelectionKind::Source:
        if (at >= recipe.sources.size()) return;
        next.holder.sources.push_back(recipe.sources[at]);
        break;
    case FluidSelectionKind::Force:
        if (at >= recipe.forces.size()) return;
        next.holder.forces.push_back(recipe.forces[at]);
        break;
    case FluidSelectionKind::Collider:
        if (at >= recipe.colliders.size()) return;
        next.holder.colliders.push_back(recipe.colliders[at]);
        break;
    default:
        return;
    }
    next.kind = list;
    /// @note 表示名は «控えた時点» のものを持つ。元の .fluid を閉じた後でもメニューに何が入っているか出す。
    next.label = fluidui::PartDisplayName(recipe, list, index);
    FluidExtrasPartClipboard() = std::move(next);
}

bool HasFluidPartClipboard()
{
    return FluidPartClipboardMatches(FluidExtrasPartClipboard().kind);
}

bool FluidPartClipboardMatches(FluidSelectionKind list)
{
    const FluidPartClipboardState& clipboard = FluidExtrasPartClipboard();
    if (clipboard.kind == FluidSelectionKind::None || clipboard.kind != list) return false;
    std::size_t count = 0;
    FluidExtrasVisitPartVector(clipboard.holder, list, [&count](const auto& parts) { count = parts.size(); });
    return count > 0;
}

FluidSelectionKind FluidPartClipboardKind()
{
    return HasFluidPartClipboard() ? FluidExtrasPartClipboard().kind : FluidSelectionKind::None;
}

std::string FluidPartClipboardLabel()
{
    return HasFluidPartClipboard() ? FluidExtrasPartClipboard().label : std::string{};
}

bool PasteFluidPartClipboard(fluid::FluidRecipe& recipe, FluidSelectionKind list, int at)
{
    if (!FluidPartClipboardMatches(list)) return false;
    const FluidPartClipboardState& clipboard = FluidExtrasPartClipboard();

    int count = 0;
    FluidExtrasVisitPartVector(recipe, list,
                               [&count](const auto& parts) { count = static_cast<int>(parts.size()); });
    if (count >= MaxParts(list)) return false;

    const int insertAt = (at < 0 || at > count) ? count : at;
    const auto offset = static_cast<std::ptrdiff_t>(insertAt);
    switch (list) {
    case FluidSelectionKind::Source:
        recipe.sources.insert(recipe.sources.begin() + offset, clipboard.holder.sources.front());
        break;
    case FluidSelectionKind::Force:
        recipe.forces.insert(recipe.forces.begin() + offset, clipboard.holder.forces.front());
        break;
    case FluidSelectionKind::Collider:
        recipe.colliders.insert(recipe.colliders.begin() + offset, clipboard.holder.colliders.front());
        break;
    default:
        return false;
    }

    std::string base;
    VisitPart(recipe, list, insertAt, [&base](const auto& part) { base = part.name; });
    const std::string unique = MakeUniqueFluidPartName(recipe, list, base, insertAt);
    if (unique != base) VisitPart(recipe, list, insertAt, [&unique](auto& part) { part.name = unique; });
    return true;
}

std::string MakeUniqueFluidPartName(const fluid::FluidRecipe& recipe, FluidSelectionKind list,
                                    const std::string& base, int ignoreIndex)
{
    /// @note 名前が空の部品は Outliner で "Source 2 (Cone)" と添字から呼ばれる。空のまま返せば衝突しない。
    if (base.empty()) return base;

    std::vector<std::string> used;
    FluidExtrasVisitPartVector(recipe, list, [&used, ignoreIndex](const auto& parts) {
        used.reserve(parts.size());
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (static_cast<int>(i) == ignoreIndex) continue;
            if (!parts[i].name.empty()) used.push_back(parts[i].name);
        }
    });
    const auto taken = [&used](const std::string& name) {
        return std::find(used.begin(), used.end(), name) != used.end();
    };
    if (!taken(base)) return base;

    const std::string stem = FluidExtrasStripNameCounter(base);
    /// @note 上限は部品の数 (16) で足りるが、名前だけ手で揃えた列に当たっても止まらない程度に取る。
    for (int serial = 1; serial < 1000; ++serial) {
        const std::string candidate = stem + " (" + std::to_string(serial) + ")";
        if (!taken(candidate)) return candidate;
    }
    return base;
}

} // namespace fbzz::editor::fluideditor
