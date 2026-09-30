/// @file    FluidEditorViewport.cpp
/// @brief   Fluid Editor のビューポート (2D ライブプレビュー・部品の重ね描き・ハンドルのドラッグ・焼き上がりとの並置)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "FluidEditorInternal.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/FluidAssetWriters.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor::fluideditor {

/// @brief 編集中のプレビューのコマ数。枠がまだ無ければ [output] から数える (0 に丸めない)。
/// @note 匿名名前空間に入れないのは、ツールバー (FluidEditorPanel.cpp) も同じ数え方をする必要があるため。
/// @note       2 か所に書くと «ビューポートとツールバーでコマ番号が違う» が静かに入り込む。
int FluidViewportLiveFrameCount(const State& state, const fluid::FluidRecipe& recipe)
{
    const int fromPreview = state.preview.FrameCount();
    if (fromPreview > 0) return fromPreview;
    return (std::max)(recipe.output.columns * recipe.output.rows, 1);
}

int FluidViewportLiveFrame(const State& state, const fluid::FluidRecipe& recipe, int frames)
{
    const float frameDt = TimelineFrameDt(state, recipe);
    const int frame = static_cast<int>(std::floor(state.playhead / frameDt + 0.5f));
    return (std::max)((std::min)(frame, (std::max)(frames, 1) - 1), 0);
}

namespace {

constexpr ImU32 kCanvasColor = IM_COL32(22, 22, 25, 255);
constexpr ImU32 kDomainBorder = IM_COL32(255, 255, 255, 48);
constexpr ImU32 kHintText = IM_COL32(200, 200, 205, 160);
constexpr ImU32 kHintTextDim = IM_COL32(170, 170, 175, 120);
constexpr float kCheckerCellPixels = 8.0f;
/// @note GPU へ上げられないときの代わりの描き方。1 コマを矩形で描くので、細かすぎると描画コマンドが膨らむ。
constexpr int kFallbackCells = 64;

/// @note 食い違いを告げる色 (ツールバーと焼き上がりの見出しで共通)。
constexpr ImVec4 kFluidCompareWarnColor{ 0.93f, 0.71f, 0.35f, 1.0f };

int ToByte(float value)
{
    return static_cast<int>(ClampF(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void DrawChecker(ImDrawList* drawList, ImVec2 min, float side)
{
    drawList->AddRectFilled(min, { min.x + side, min.y + side }, FluidPreviewBackgroundPixel(0, 0, true));
    const int cells = static_cast<int>(std::ceil(side / kCheckerCellPixels));
    for (int y = 0; y < cells; ++y) {
        for (int x = 0; x < cells; ++x) {
            const ImU32 background = FluidPreviewBackgroundPixel(x * 8, y * 8, true);
            if (background == FluidPreviewBackgroundPixel(0, 0, true)) continue;
            const ImVec2 a{ min.x + static_cast<float>(x) * kCheckerCellPixels,
                            min.y + static_cast<float>(y) * kCheckerCellPixels };
            const ImVec2 b{ (std::min)(a.x + kCheckerCellPixels, min.x + side),
                            (std::min)(a.y + kCheckerCellPixels, min.y + side) };
            drawList->AddRectFilled(a, b, background);
        }
    }
}

void DrawFrameFallback(ImDrawList* drawList, const asset::FluidFrameImage& frame, fluid::FluidShading shading,
                       bool checkerBackground, ImVec2 min, float side)
{
    const int size = frame.size;
    if (size <= 0 || frame.rgba.size() < static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u) return;
    const int cells = (std::min)(size, kFallbackCells);
    const float cell = side / static_cast<float>(cells);
    for (int cy = 0; cy < cells; ++cy) {
        const int py = (std::min)(size - 1, (cy * size + size / 2) / cells);
        for (int cx = 0; cx < cells; ++cx) {
            const int px = (std::min)(size - 1, (cx * size + size / 2) / cells);
            const float* p = &frame.rgba[(static_cast<std::size_t>(py) * static_cast<std::size_t>(size)
                                          + static_cast<std::size_t>(px)) * 4u];
            float color[3];
            CompositeFluidPreviewPixel(p, px, py, size, side, shading, checkerBackground, color);
            const ImVec2 a{ min.x + static_cast<float>(cx) * cell, min.y + static_cast<float>(cy) * cell };
            drawList->AddRectFilled(a, { a.x + cell, a.y + cell },
                                    IM_COL32(ToByte(color[0]), ToByte(color[1]), ToByte(color[2]), 255));
        }
    }
}

const char* UndoLabelFor(FluidHandleKind kind)
{
    switch (kind) {
    case FluidHandleKind::Center:    return "Move Fluid Part";
    case FluidHandleKind::Size:      return "Resize Fluid Part";
    case FluidHandleKind::Direction: return "Rotate Fluid Part";
    case FluidHandleKind::MotionKey: return "Move Fluid Motion Key";
    }
    return "Move Fluid Part";
}

/// @note ハンドルの奥行き (2D のビューポートは z を見ないので、今の z をそのまま返してもらう)。
float HandleDepth(const fluid::FluidRecipe& recipe, const FluidPartHandle& handle, float time)
{
    float depth = 0.0f;
    const float solverTime = recipe.output.warmup + time;
    VisitPart(recipe, handle.list, handle.index, [&](const auto& part) {
        depth = part.center.z;
        if (handle.kind == FluidHandleKind::MotionKey) {
            if (handle.keyIndex >= 0 && handle.keyIndex < static_cast<int>(part.motion.keys.size()))
                depth += part.motion.keys[static_cast<std::size_t>(handle.keyIndex)].offset.z;
        } else {
            depth += MotionOffsetAt(part.motion, solverTime).z;
        }
    });
    return depth;
}

ImGuiMouseCursor CursorFor(FluidHandleKind kind)
{
    return kind == FluidHandleKind::Center ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand;
}

/// @note 枠の中に置く «ドメインの正方形»。編集中と焼き上がりで同じ式を使う — 並べて比べるものなので、
/// @note 片方だけ大きさや位置が動くと «違い» が形の違いなのか置き方の違いなのか分からなくなる。
struct FluidViewportSquare {
    ImVec2 min{ 0.0f, 0.0f };
    ImVec2 center{ 0.0f, 0.0f };
    float side = 0.0f;
};

FluidViewportSquare FluidViewportSquareFor(const State& state, ImVec2 canvasMin, ImVec2 canvasSize)
{
    FluidViewportSquare square;
    square.center = { canvasMin.x + canvasSize.x * 0.5f, canvasMin.y + canvasSize.y * 0.5f };
    square.side = (std::max)((std::min)(canvasSize.x, canvasSize.y) * 0.96f * state.zoom, 8.0f);
    square.min = { square.center.x + state.pan.x - square.side * 0.5f,
                   square.center.y + state.pan.y - square.side * 0.5f };
    return square;
}

/// @note 正方形の真ん中へ 1〜2 行の案内を置く。
void FluidViewportCenteredHint(ImDrawList* drawList, ImVec2 min, float side, const char* title,
                               const char* hint)
{
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 center{ min.x + side * 0.5f, min.y + side * 0.5f };
    const int lines = hint != nullptr ? 2 : 1;
    float y = center.y - lineHeight * static_cast<float>(lines) * 0.5f;
    const auto line = [&](const char* text, ImU32 color) {
        if (text == nullptr) return;
        const ImVec2 size = ImGui::CalcTextSize(text);
        drawList->AddText({ center.x - size.x * 0.5f, y }, color, text);
        y += lineHeight;
    };
    line(title, kHintText);
    line(hint, kHintTextDim);
}

/// @name 焼き上がりとの並置
/// @note 名前を長く取るのは、Editor が Unity ビルド (複数の .cpp が 1 翻訳単位へ入る) で
/// @note       無名名前空間の名前が隣のファイルと衝突しうるため。Baked タブの状態は借りない —
/// @note       あちらは «このセッションで焼いた 1 件» のみを持ち、開き直しただけでは空になる。

/// @note 枠 1 つの下限 [画面画素]。これより狭いところへ並べると、どちらの絵も読めなくなる。
constexpr float kFluidComparePaneMin = 180.0f;
/// @note 焼き直し (と消えたこと) を見つけるための見直し間隔 [秒]。
constexpr float kFluidCompareRescanInterval = 0.5f;

enum class FluidComparePlacement : std::uint8_t { Single, SideBySide, Stacked };

/// @note 並べ方を決める。パネルは 3 ペインなので中央列は狭くなり得る。潰れるくらいなら縦に積み、
/// @note それも無理なら単独表示へ戻す。
FluidComparePlacement FluidViewportResolveComparePlacement(bool wanted, ImVec2 region)
{
    if (!wanted) return FluidComparePlacement::Single;
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    if ((region.x - spacing.x) * 0.5f >= kFluidComparePaneMin) return FluidComparePlacement::SideBySide;
    /// @note 縦に積むと枠ごとの見出しが 1 行ずつ要る。
    const float header = ImGui::GetTextLineHeightWithSpacing();
    if ((region.y - spacing.y) * 0.5f - header >= kFluidComparePaneMin)
        return FluidComparePlacement::Stacked;
    return FluidComparePlacement::Single;
}

/// @note 液体は Liquid でしか描けず、気体に Liquid を指定しても描けない (FluidPreviewCache と同じ丸め)。
fluid::FluidShading FluidViewportEffectiveShading(const fluid::FluidRecipe& recipe)
{
    if (recipe.kind == fluid::FluidKind::Liquid) return fluid::FluidShading::Liquid;
    return recipe.render.shading == fluid::FluidShading::Liquid ? fluid::FluidShading::Smoke
                                                                : recipe.render.shading;
}

/// @brief .fluid の隣に焼かれた Atlas の居場所と、焼いたときのコマ割り。
/// @note GPU 資源は持たない。ビューポートに終了の口が無く (OnShutdown は Baked タブとプレビュー
/// @note       キャッシュだけを畳む) 自前で作ると返す先が無いため、ResourceManager のパスキャッシュ
/// @note       (LoadTexture) に借りるだけにする。
struct FluidViewportBakedAtlas {
    std::string fluidPath;
    std::string atlasPath;       ///< 実パス (更新時刻を見る)
    std::string atlasAssetPath;  ///< Assets 起点 (LoadTexture のキー)
    bool found = false;
    bool volume = false;
    /// @note 3D と Fire / Glow の Atlas は事前乗算。ImGui はストレートアルファで重ねるので縁が濃く出る。
    bool premultiplied = false;
    int columns = 1;
    int rows = 1;
    int frameCount = 1;
    /// @note 1 コマの画素 ([output] の Frame Size)。.mat が無いとき Atlas の寸法から数え直すのに使う。
    int frameSize = 0;
    /// @note 焼いたときの尺 [秒]。
    float duration = 0.0f;
    /// @note コマ割りを隣の .mat (焼きが書いたもの) から取れたか。
    bool gridFromMaterial = false;
    std::filesystem::file_time_type stamp{};
    float pollTimer = 0.0f;

    renderer::ResourceHandle<renderer::TextureTag> texture;
    std::uint64_t resetVersion = 0;
    /// @note 一度読みに行ったか。読めないパスを毎フレーム引き直すとログが埋まる。
    bool textureTried = false;
};

FluidViewportBakedAtlas& FluidViewportBakedAtlasState()
{
    static FluidViewportBakedAtlas baked;
    return baked;
}

void FluidViewportScanBakedAtlas(State& state, FluidViewportBakedAtlas& baked)
{
    const std::string fluidPath = state.document.Path();
    baked = FluidViewportBakedAtlas{};
    baked.fluidPath = fluidPath;
    if (fluidPath.empty()) return;

    const std::string stem =
        util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(fluidPath).replace_extension());

    struct Candidate {
        std::string path;
        bool volume;
    };
    /// @note 名前は Baked タブと同じ規則 (3D は `<stem>`.png、2D は `<stem>`_Flipbook.png)。PNG が原本なので先に見る。
    std::vector<Candidate> candidates;
    if (state.document.Recipe().bake.mode == fluid::FluidBakeMode::Volume3D) {
        candidates.push_back({ stem + ".png", true });
        candidates.push_back({ stem + ".dds", true });
    }
    /// @note _Flipbook は焼き以外が付けない名前なので、モードを問わず見る (3D へ変えただけで «焼いていない»
    /// @note       顔をしないように)。逆に `<stem>`.png は隣に置かれた別の絵かもしれないので 3D のときしか見ない。
    candidates.push_back({ stem + "_Flipbook.png", false });
    candidates.push_back({ stem + "_Flipbook.dds", false });
    for (const Candidate& candidate : candidates) {
        if (!util::FileSystem::Exists(candidate.path)) continue;
        baked.atlasPath = candidate.path;
        baked.volume = candidate.volume;
        baked.found = true;
        break;
    }
    if (!baked.found) return;
    baked.atlasAssetPath = NormalizeAssetPath(baked.atlasPath);
    baked.stamp = util::FileSystem::LastWriteTime(util::FileSystem::PathFromUtf8(baked.atlasPath));

    /// @note 焼いたときの値はディスクの .fluid から読む。編集中のレシピは «焼いた後に変えたぶん» だけずれており、
    /// @note       そのずれこそ並置で見たいもの。
    fluid::FluidRecipe onDisk;
    const fluid::FluidRecipe& source =
        asset::LoadFluidRecipe(fluidPath, onDisk) ? onDisk : state.document.Recipe();
    baked.frameSize = (std::max)(source.output.frameSize, 1);
    baked.columns = (std::max)(source.output.columns, 1);
    baked.rows = (std::max)(source.output.rows, 1);
    baked.frameCount = baked.columns * baked.rows;
    baked.duration = TimelineDuration(source);
    baked.premultiplied =
        baked.volume || asset::FluidShadingIsPremultiplied(FluidViewportEffectiveShading(source));

    /// @note 隣の .mat は «焼いた結果そのもの» を持っている (Baked タブと同じ読み方)。
    const std::string materialPath = SiblingMaterialPath(fluidPath);
    asset::MaterialAsset material;
    if (materialPath.empty() || !util::FileSystem::Exists(materialPath)
        || !asset::LoadMaterialAssetFromFile(materialPath, material))
        return;
    const asset::ParticleFlipbookSettings& flipbook = material.particle.flipbook;
    if (flipbook.FrameCount() > 1) {
        baked.columns = (std::max)(flipbook.spriteColumns, 1);
        baked.rows = (std::max)(flipbook.spriteRows, 1);
        baked.frameCount = flipbook.FrameCount();
        baked.gridFromMaterial = true;
    }
    if (flipbook.flipbookMode == scene::ParticleFlipbookMode::FramesPerSecond)
        baked.duration = static_cast<float>(baked.frameCount)
                       / (std::max)(flipbook.flipbookFramesPerSecond, 0.1f);
}

/// @note 焼き上がりを «今のディスクの姿» に合わせる。焼き直し・消えた・別の .fluid を開いたを拾う。
FluidViewportBakedAtlas& FluidViewportRefreshBakedAtlas(EditorContext& ctx, State& state, bool force)
{
    FluidViewportBakedAtlas& baked = FluidViewportBakedAtlasState();
    const std::string fluidPath = state.document.Path();
    bool rescan = force || fluidPath != baked.fluidPath;
    if (!rescan) {
        baked.pollTimer += ImGui::GetIO().DeltaTime;
        if (baked.pollTimer >= kFluidCompareRescanInterval) {
            baked.pollTimer = 0.0f;
            /// @note まだ焼かれていない間も見直す。焼けた瞬間に並びたい。
            rescan = !baked.found || !util::FileSystem::Exists(baked.atlasPath)
                  || util::FileSystem::LastWriteTime(util::FileSystem::PathFromUtf8(baked.atlasPath))
                         != baked.stamp;
        }
    }
    if (!rescan) return baked;

    const std::string previous = baked.atlasAssetPath;
    FluidViewportScanBakedAtlas(state, baked);
    if (ctx.resources == nullptr) return baked;
    baked.resetVersion = ctx.resources->GetResetVersion();
    /// @note 同じ名前へ焼き直したときは、パスキャッシュが前の絵を返し続ける。
    if (baked.found && !previous.empty() && baked.atlasAssetPath == previous)
        baked.texture = ctx.resources->ReloadTexture(baked.atlasAssetPath);
    return baked;
}

/// @note 焼き上がりの Atlas を ImGui へ渡せる形で借りる。寸法も返す (コマ割りの検算に使う)。
ImTextureID FluidViewportBakedTexture(EditorContext& ctx, FluidViewportBakedAtlas& baked,
                                      std::uint32_t& outWidth, std::uint32_t& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    if (!baked.found || ctx.resources == nullptr || ctx.imguiRenderer == nullptr) return ImTextureID{};

    if (const std::uint64_t resetVersion = ctx.resources->GetResetVersion();
        resetVersion != baked.resetVersion) {
        /// @note デバイスリセットで前のハンドルは無効。1 回だけ引き直す。
        baked.resetVersion = resetVersion;
        baked.texture = {};
        baked.textureTried = false;
    }
    if (baked.texture.IsValid() && ctx.resources->Get(baked.texture) == nullptr) {
        /// @note 誰かがキャッシュから外した (ホットリロード・削除)。引き直しは 1 回だけ許す。
        baked.texture = {};
        baked.textureTried = false;
    }
    if (!baked.texture.IsValid()) {
        if (baked.textureTried) return ImTextureID{};
        baked.textureTried = true;
        baked.texture = ctx.resources->LoadTexture(baked.atlasAssetPath);
    }
    renderer::ITexture* gpu = ctx.resources->Get(baked.texture);
    if (gpu == nullptr) return ImTextureID{};
    outWidth = gpu->GetWidth();
    outHeight = gpu->GetHeight();
    /// @note ID を覚え込まない。DX12 は «画素として読む» 状態への移行をここで積む。
    void* rawId = ctx.imguiRenderer->GetImTextureID(baked.texture, *ctx.resources);
    if (rawId == nullptr) return ImTextureID{};
    return widgets::ToImTextureID(rawId);
}

struct FluidViewportAtlasGrid {
    int columns = 1;
    int rows = 1;
    int frameCount = 1;
    /// @note コマ割りで Atlas を割り切れたか。割り切れないまま 1 コマを切り出すと «別のコマ» が出る。
    bool fitsAtlas = true;
    /// @note .mat / .fluid の値では割り切れず、Atlas の寸法から数え直した。
    bool guessed = false;
};

FluidViewportAtlasGrid FluidViewportResolveAtlasGrid(const FluidViewportBakedAtlas& baked,
                                                    std::uint32_t width, std::uint32_t height)
{
    FluidViewportAtlasGrid grid;
    grid.columns = (std::max)(baked.columns, 1);
    grid.rows = (std::max)(baked.rows, 1);
    grid.frameCount = (std::max)(baked.frameCount, 1);

    const auto fits = [width, height](int columns, int rows) {
        if (columns <= 0 || rows <= 0 || width == 0 || height == 0) return false;
        const auto c = static_cast<std::uint32_t>(columns);
        const auto r = static_cast<std::uint32_t>(rows);
        return width % c == 0 && height % r == 0 && width / c == height / r;
    };
    const auto tile = baked.frameSize > 0 ? static_cast<std::uint32_t>(baked.frameSize) : 0u;
    const bool divides = fits(grid.columns, grid.rows);

    /// @note .mat のコマ割りは «焼きが書いた値» なので、割り切れればそれが正しい。.fluid の今の値はそうではない:
    /// @note       コマ割りだけ変えて保存すると、たまたま割り切れて (2048² を 8x8 ではなく 4x4 と読む) 黙って別の
    /// @note       コマを切り出す。1 コマの大きさで検算してから採る。
    if (divides
        && (baked.gridFromMaterial || tile == 0u
            || width / static_cast<std::uint32_t>(grid.columns) == tile))
        return grid;

    if (tile != 0u) {
        const int columns = static_cast<int>(width / tile);
        const int rows = static_cast<int>(height / tile);
        if (fits(columns, rows)) {
            grid.columns = columns;
            grid.rows = rows;
            grid.frameCount = columns * rows;
            grid.guessed = true;
            return grid;
        }
    }
    if (divides) {
        /// @note 割り切れはする。«焼いたときの値» とは言い切れないので怪しいと出す。
        grid.guessed = true;
        return grid;
    }
    grid.fitsAtlas = false;
    return grid;
}

/// @note 再生位置の突き合わせ。編集中のコマ番号が正本で、焼き上がりはそれに合わせる。
struct FluidViewportFrameMatch {
    int frame = 0;
    int bakedFrames = 1;
    int liveFrames = 1;
    /// @note コマ数が違うので割合で合わせた (コマ単位では突き合わせられない)。
    bool ratio = false;
};

FluidViewportFrameMatch FluidViewportMatchFrame(const State& state, const fluid::FluidRecipe& recipe,
                                                int bakedFrames)
{
    FluidViewportFrameMatch match;
    match.bakedFrames = (std::max)(bakedFrames, 1);
    match.liveFrames = FluidViewportLiveFrameCount(state, recipe);
    const int liveFrame = FluidViewportLiveFrame(state, recipe, match.liveFrames);
    if (match.bakedFrames == match.liveFrames) {
        match.frame = liveFrame;
        return match;
    }
    match.ratio = true;
    if (match.liveFrames <= 1) return match;
    const float ratio = static_cast<float>(liveFrame) / static_cast<float>(match.liveFrames - 1);
    match.frame = (std::min)(static_cast<int>(ratio * static_cast<float>(match.bakedFrames - 1) + 0.5f),
                             match.bakedFrames - 1);
    return match;
}

/// @note 焼き上がりの枠。編集中の枠と同じ大きさ・同じズームで 1 コマだけ出す。
void FluidViewportDrawBakedPane(EditorContext& ctx, State& state)
{
    const fluid::FluidRecipe& recipe = state.document.Recipe();
    FluidViewportBakedAtlas& baked = FluidViewportRefreshBakedAtlas(ctx, state, false);

    std::uint32_t atlasWidth = 0;
    std::uint32_t atlasHeight = 0;
    const ImTextureID texture = FluidViewportBakedTexture(ctx, baked, atlasWidth, atlasHeight);
    const bool hasImage = texture != ImTextureID{};
    const FluidViewportAtlasGrid grid = FluidViewportResolveAtlasGrid(baked, atlasWidth, atlasHeight);
    const FluidViewportFrameMatch match = FluidViewportMatchFrame(state, recipe, grid.frameCount);
    const float liveDuration = TimelineDuration(recipe);
    const bool durationDiffers =
        std::fabs(baked.duration - liveDuration) > (std::max)(liveDuration * 0.01f, 1.0e-3f);
    const bool mismatched =
        hasImage && (match.ratio || durationDiffers || grid.guessed || !grid.fitsAtlas);

    /// @name 見出し
    /// @note 食い違いはここで言う。絵だけ見て «別のコマを並べている» とは気付けない。
    if (hasImage) {
        ImGui::TextDisabled("Baked  frame %d / %d", match.frame + 1, match.bakedFrames);
        if (baked.premultiplied)
            ImGui::SetItemTooltip("この Atlas は事前乗算で焼いてある。ここは ImGui のストレートアルファで"
                                  "重ねるので、薄い縁だけ編集中の絵より濃く出ます (形と動きは同じ)");
    } else {
        ImGui::TextDisabled("Baked");
    }
    ImGui::SameLine();
    if (mismatched) {
        ImGui::TextColored(kFluidCompareWarnColor, "(not aligned)");
        if (ImGui::BeginItemTooltip()) {
            if (match.ratio)
                ImGui::Text("Baked %d frames, editing %d: the playhead is matched by ratio.",
                            match.bakedFrames, match.liveFrames);
            if (durationDiffers)
                ImGui::Text("Baked %.2f s, editing %.2f s.", baked.duration, liveDuration);
            if (grid.guessed)
                ImGui::Text("The %d x %d frame grid was worked out from the atlas: no baked .mat to read it"
                            " from, and the .fluid was edited since.", grid.columns, grid.rows);
            if (!grid.fitsAtlas)
                ImGui::Text("A %d x %d grid does not divide the %u x %u atlas, so the whole sheet is shown.",
                            grid.columns, grid.rows, static_cast<unsigned>(atlasWidth),
                            static_cast<unsigned>(atlasHeight));
            ImGui::Text("Bake again to compare frame by frame.");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Reload##fe_cmp_reload")) FluidViewportRefreshBakedAtlas(ctx, state, true);
    ImGui::SetItemTooltip("%s", baked.found ? baked.atlasAssetPath.c_str()
                                            : ".fluid の隣の焼き上がりをもう一度探す");

    /// @name 絵
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize{ (std::max)(avail.x, 32.0f), (std::max)(avail.y, 32.0f) };
    const ImVec2 canvasMax{ canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(canvasMin, canvasMax, true);
    drawList->AddRectFilled(canvasMin, canvasMax, kCanvasColor);

    const FluidViewportSquare square = FluidViewportSquareFor(state, canvasMin, canvasSize);
    const ImVec2 squareMax{ square.min.x + square.side, square.min.y + square.side };
    if (state.checkerBackground) DrawChecker(drawList, square.min, square.side);
    else drawList->AddRectFilled(square.min, squareMax, FluidPreviewBackgroundPixel(0, 0, false));

    if (hasImage) {
        ImVec2 uv0{ 0.0f, 0.0f };
        ImVec2 uv1{ 1.0f, 1.0f };
        if (grid.fitsAtlas) {
            /// @note Atlas は左上から行優先 (FlipbookGrid の規約)。
            const int column = match.frame % grid.columns;
            const int row = match.frame / grid.columns;
            uv0 = { static_cast<float>(column) / static_cast<float>(grid.columns),
                    static_cast<float>(row) / static_cast<float>(grid.rows) };
            uv1 = { uv0.x + 1.0f / static_cast<float>(grid.columns),
                    uv0.y + 1.0f / static_cast<float>(grid.rows) };
        }
        drawList->AddImage(ImTextureRef(texture), square.min, squareMax, uv0, uv1);
    } else if (!baked.found) {
        FluidViewportCenteredHint(drawList, square.min, square.side, "Not baked yet",
                                  baked.fluidPath.empty()
                                      ? "Save this .fluid, then bake it."
                                      : "The bake writes the atlas next to the .fluid.");
    } else {
        FluidViewportCenteredHint(drawList, square.min, square.side, "Cannot show the baked atlas",
                                  "Press Reload, or open the Baked tab.");
    }
    drawList->AddRect(square.min, squareMax, kDomainBorder);
    drawList->PopClipRect();
}

/// @note 2D のライブプレビュー (市松 → コマ → 重ね描き) と、つかむ・動かす・視点。
void FluidViewportDrawEditCanvas2D(State& state)
{
    FluidDocument& document = state.document;
    const fluid::FluidRecipe& recipe = document.Recipe();
    const ImGuiIO& io = ImGui::GetIO();

    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize{ (std::max)(avail.x, 32.0f), (std::max)(avail.y, 32.0f) };
    const ImVec2 canvasMax{ canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
    ImGui::InvisibleButton("##fe_canvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(canvasMin, canvasMax, true);
    drawList->AddRectFilled(canvasMin, canvasMax, kCanvasColor);

    const FluidViewportSquare square = FluidViewportSquareFor(state, canvasMin, canvasSize);
    /// @note 画像の細かさはこの一辺に合わせる (解いて描くのは裏のスレッドなので、効くのは次のコマから)。
    state.previewViewSide = square.side;
    FluidViewMapping mapping;
    mapping.origin = square.min;
    mapping.size = square.side;

    DrawFluidPreviewSquare(drawList, state, square.min, square.side,
                           state.preview.IsSolving() ? "Solving..." : "No preview frame yet");

    const FluidPartVisibility visible = [&document](FluidSelectionKind list, int index) {
        return PartShownInPreview(document, list, index);
    };
    if (state.showOverlays)
        DrawFluidPartOverlays(drawList, mapping, recipe, state.playhead, document.selection, visible);
    drawList->PopClipRect();

    const ImVec2 mouse = io.MousePos;

    /// @name つかむ
    if (state.showOverlays && !state.viewDrag.active) {
        const std::vector<FluidPartHandle> handles =
            CollectFluidPartHandles(mapping, recipe, state.playhead, document.selection, visible);
        const FluidPartHandle* over = hovered ? PickFluidPartHandle(handles, mouse) : nullptr;
        if (over != nullptr) ImGui::SetMouseCursor(CursorFor(over->kind));

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (over != nullptr) {
                document.selection = FluidSelection{ over->list, over->index };
                ViewportDrag drag;
                drag.active = true;
                drag.handle = *over;
                drag.pressMouse = mouse;
                drag.lastMouse = mouse;
                drag.lastShift = io.KeyShift;
                drag.grabOffset = { mouse.x - over->screen.x, mouse.y - over->screen.y };
                drag.undoLabel = UndoLabelFor(over->kind);
                state.viewDrag = drag;
                /// @note 掴んだだけで再生は止めない (止めるのはスクラブ・コマ送りだけ)。
                document.BeginInteractiveEdit();
            } else {
                const FluidSelection picked = PickFluidPart(mapping, recipe, state.playhead, mouse, visible);
                if (picked.kind != FluidSelectionKind::None) document.selection = picked;
            }
        }
    }

    /// @name 動かす (離したときの Undo は EndStaleDrags が積む)
    if (state.viewDrag.active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && document.InInteractiveEdit()) {
        ViewportDrag& drag = state.viewDrag;
        ImGui::SetMouseCursor(CursorFor(drag.handle.kind));
        const ImVec2 delta{ mouse.x - drag.pressMouse.x, mouse.y - drag.pressMouse.y };
        const bool mouseMoved = mouse.x != drag.lastMouse.x || mouse.y != drag.lastMouse.y;
        const bool shiftChanged = io.KeyShift != drag.lastShift;
        const bool pastThreshold = drag.moved || std::fabs(delta.x) + std::fabs(delta.y) >= 1.0f;
        if (pastThreshold && (mouseMoved || shiftChanged || !drag.moved)) {
            ImVec2 target{ mouse.x - drag.grabOffset.x, mouse.y - drag.grabOffset.y };
            /// @note Shift は押した位置から見て大きく動いた軸だけを残す。
            if (io.KeyShift) {
                if (std::fabs(delta.x) >= std::fabs(delta.y))
                    target.y = drag.handle.screen.y;
                else
                    target.x = drag.handle.screen.x;
            }
            math::Vector3 domain = mapping.ToDomain(target);
            domain.z = HandleDepth(recipe, drag.handle, state.playhead);
            fluid::FluidRecipe working = recipe;
            ApplyFluidHandleDrag(working, drag.handle, domain, state.playhead);
            document.ApplyInteractive(working);
            drag.moved = true;
            drag.lastMouse = mouse;
            drag.lastShift = io.KeyShift;
        }
    }

    /// @name 視点
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) state.panning = true;
    if (state.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            state.pan.x += io.MouseDelta.x;
            state.pan.y += io.MouseDelta.y;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            state.panning = false;
        }
    }
    if (hovered && io.MouseWheel != 0.0f && !state.viewDrag.active) {
        const float before = state.zoom;
        state.zoom = ClampF(state.zoom * std::pow(1.15f, io.MouseWheel), 0.25f, 8.0f);
        /// @note カーソルの下の点を動かさずに拡大する。
        const float ratio = state.zoom / before;
        const ImVec2 squareCenter{ square.center.x + state.pan.x, square.center.y + state.pan.y };
        state.pan.x = mouse.x - (mouse.x - squareCenter.x) * ratio - square.center.x;
        state.pan.y = mouse.y - (mouse.y - squareCenter.y) * ratio - square.center.y;
    }
}

/// @note 編集中の側。単独表示でも並置でも同じものを出す。
void FluidViewportDrawEditPane(EditorContext& ctx, State& state)
{
    if (state.viewMode == ViewportMode::Volume3D) {
        DrawViewport3D(ctx, state);
        return;
    }
    FluidViewportDrawEditCanvas2D(state);
}

}

void DrawFluidPreviewSquare(ImDrawList* drawList, State& state, ImVec2 min, float side, const char* emptyText)
{
    state.preview.SetPreviewSide(state.previewViewSide);
    state.preview.SetCheckerBackground(state.checkerBackground);
    const ImVec2 max{ min.x + side, min.y + side };
    if (state.checkerBackground)
        DrawChecker(drawList, min, side);
    else
        drawList->AddRectFilled(min, max, FluidPreviewBackgroundPixel(0, 0, false));

    const ImTextureID texture = state.preview.TextureAt(state.playhead);
    fluid::FluidShading shading = fluid::FluidShading::Smoke;
    if (texture != ImTextureID{}) {
        drawList->AddImage(ImTextureRef(texture), min, max);
    } else if (const asset::FluidFrameImage* image = state.preview.FrameAt(state.playhead, &shading)) {
        DrawFrameFallback(drawList, *image, shading, state.checkerBackground, min, side);
    } else if (emptyText != nullptr) {
        const ImVec2 textSize = ImGui::CalcTextSize(emptyText);
        drawList->AddText({ (min.x + max.x - textSize.x) * 0.5f, (min.y + max.y - textSize.y) * 0.5f }, kHintText,
                          emptyText);
    }
    drawList->AddRect(min, max, kDomainBorder);
}

void DrawViewport(EditorContext& ctx, State& state)
{
    const fluid::FluidRecipe& recipe = state.document.Recipe();

    /// @note 並べ方は道具の並びを描く «前» に決める。狭くて並べられないことをツールバーで言うため。
    ImVec2 region = ImGui::GetContentRegionAvail();
    region.y -= ImGui::GetFrameHeightWithSpacing();
    const FluidComparePlacement placement =
        FluidViewportResolveComparePlacement(state.compareBaked, region);

    /// @note 開いた直後は «そのレシピが焼かれる形» を映す。人が切り替えたらもう戻さない。
    if (!state.viewModeChosen) {
        state.viewMode = recipe.bake.mode == fluid::FluidBakeMode::Volume3D ? ViewportMode::Volume3D
                                                                           : ViewportMode::Flat2D;
        state.viewModeChosen = true;
    }
    ImGui::TextDisabled("Preview:");
    ImGui::SameLine();
    if (ImGui::RadioButton("2D##fe_view2d", state.viewMode == ViewportMode::Flat2D))
        state.viewMode = ViewportMode::Flat2D;
    ImGui::SetItemTooltip("2D の平面シミュレーションをプレビューします。書き出し形式は Bake Mode で決まります。");
    ImGui::SameLine();
    if (ImGui::RadioButton("3D##fe_view3d", state.viewMode == ViewportMode::Volume3D))
        state.viewMode = ViewportMode::Volume3D;
    ImGui::SetItemTooltip("3D 体積のライブレイマーチです。視点を回せるのは編集プレビューだけです。");

    /// @note 2D と 3D は «同じものの別の見せ方» ではない。解く次元もレンダラーも別なので絵は必ず違う。
    const bool bakes3D = recipe.bake.mode == fluid::FluidBakeMode::Volume3D;
    const bool viewing3D = state.viewMode == ViewportMode::Volume3D;
    ImGui::TextDisabled("Bake output: %s", bakes3D ? "3D single-view billboard flipbook" : "2D flipbook");
    ImGui::SetItemTooltip(bakes3D
        ? "Bake > Mode selects this output. The volume bake renders a single fixed view into a flipbook shown on camera-facing billboards; it is not view-dependent 3D. Orbit changes only the live 3D preview."
        : "Bake > Mode selects this output. The 2D bake writes a flat flipbook; the 3D preview is a separate volume raymarch view.");
    if (bakes3D != viewing3D) {
        ImGui::TextColored(kFluidCompareWarnColor, "Preview differs from Bake");
        ImGui::SetItemTooltip("Preview and Bake use different simulation dimensions. Change Bake > Mode to change the output, or switch Preview to inspect the bake mode.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Match##fe_view_match"))
            state.viewMode = bakes3D ? ViewportMode::Volume3D : ViewportMode::Flat2D;
        ImGui::SetItemTooltip("Switch the live preview to the Bake Mode");
    }

    ImGui::Checkbox("Checker", &state.checkerBackground);
    ImGui::SetItemTooltip("背景を市松模様にする (透け具合を見る)。外すと暗い無地");
    ImGui::SameLine();
    ImGui::Checkbox("Overlays", &state.showOverlays);
    ImGui::SetItemTooltip("部品の形・向き・動きの道筋とハンドルを重ねる");
    ImGui::SameLine();
    ImGui::Checkbox("Compare##fe_compare", &state.compareBaked);
    ImGui::SetItemTooltip("編集中のプレビューの隣に «焼き上がり» (.fluid の隣の Atlas) を並べる。"
                          "再生位置は編集中の側に合わせます");
    if (state.compareBaked && placement == FluidComparePlacement::Single) {
        ImGui::SameLine();
        ImGui::TextColored(kFluidCompareWarnColor, "(too small)");
        ImGui::SetItemTooltip("ビューポートが狭いので単独表示に戻しています (幅か高さを広げてください)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) {
        state.zoom = 1.0f;
        state.pan = { 0.0f, 0.0f };
    }
    ImGui::SetItemTooltip("ズームと位置を戻す (ホイールでズーム、中ボタンのドラッグで移動)");
    ImGui::SameLine();
    {
        const int frames = FluidViewportLiveFrameCount(state, recipe);
        ImGui::TextDisabled("t %.3f s | frame %d / %d | zoom %.0f%%%s", state.playhead,
                            FluidViewportLiveFrame(state, recipe, frames) + 1, frames,
                            state.zoom * 100.0f, state.preview.IsSolving() ? " | solving..." : "");
    }

    if (placement == FluidComparePlacement::Single) {
        FluidViewportDrawEditPane(ctx, state);
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    /// @note 両方に同じ大きさを渡す。片方を «残り全部» にすると丸めで 1 画素ずれ、絵の大きさが揃わない。
    const ImVec2 paneSize = placement == FluidComparePlacement::SideBySide
        ? ImVec2{ (std::max)((avail.x - spacing.x) * 0.5f, 32.0f), (std::max)(avail.y, 32.0f) }
        : ImVec2{ (std::max)(avail.x, 32.0f), (std::max)((avail.y - spacing.y) * 0.5f, 32.0f) };
    constexpr ImGuiWindowFlags paneFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginChild("##fe_pane_editing", paneSize, ImGuiChildFlags_None, paneFlags)) {
        const int frames = FluidViewportLiveFrameCount(state, recipe);
        ImGui::TextDisabled("Editing  frame %d / %d", FluidViewportLiveFrame(state, recipe, frames) + 1,
                            frames);
        FluidViewportDrawEditPane(ctx, state);
    }
    ImGui::EndChild();
    if (placement == FluidComparePlacement::SideBySide) ImGui::SameLine();
    if (ImGui::BeginChild("##fe_pane_baked", paneSize, ImGuiChildFlags_None, paneFlags))
        FluidViewportDrawBakedPane(ctx, state);
    ImGui::EndChild();
}

}
