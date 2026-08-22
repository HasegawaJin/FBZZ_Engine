// FBZZ Engine
// UISystem.cpp | fbzz::scene
// ランタイム UI の描画とボタン入力処理
// UICanvas / UIImage / UIText / UIButton を走査し、DrawCall と hit 状態を作る。
// ScreenSpace / ScreenSpaceCamera / WorldSpace の 3 モードを扱う。
#include "Engine/Scene/Systems/UISystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/UICanvas.hpp"
#include "Engine/Scene/Components/UIImage.hpp"
#include "Engine/Scene/Components/UIButton.hpp"
#include "Engine/Scene/Components/UIText.hpp"
#include "Engine/Scene/Components/UILayoutGroup.hpp"
#include "Engine/Scene/Components/UIControls.hpp"
#include "Engine/Input/Input.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/MaterialParamBinding.hpp"
#include "Engine/Asset/TextureAsset.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/FontAtlas.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ITexture.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderLayer.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Math/Matrix4.hpp"
#include "Math/Vector4.hpp"
#include "Math/Ray.hpp"
#include "Math/Plane.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Util/Utf8.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

// 頂点レイアウトの定義は UISystem.hpp (UIVertex2D)。作業領域を Context へ
// 置くためにヘッダー側へ出してあるので、ここでは名前だけ借りる。
using UIVertex = UIVertex2D;

// LAYOUT: Assets/Shaders/UI/UICommon.hlsli の cbuffer UIConstants と一致させること。
struct UIConstants {
    math::Matrix4 ortho;
    math::Vector4 color;
    math::Vector4 uvRect;
    // xy = この矩形の Canvas ピクセル寸法, zw = 予約。
    // WHY 渡すか: 角丸・枠線・影はすべて「何ピクセルぶん」で決まる。UV しか無いと
    //     同じ半径指定でも矩形の縦横比で角の形が変わり、部品を使い回せない。
    math::Vector4 rect;
};

struct CanvasEntry {
    GameObject* go = nullptr;
    UICanvas* canvas = nullptr;
};

struct CanvasRuntimeState {
    math::Matrix4 canvasToClip = math::Matrix4::Identity();
    math::Vector2 mouseInCanvasSpace = {};
    renderer::ResourceHandle<renderer::PipelineStateTag> pso;
    renderer::RenderLayer layer = renderer::RenderLayer::OVERLAY_LAYER;
};

// Fixed VB capacities
//
// WHY 画像 1 枚に 24 頂点を確保するか: 通常の矩形は 6 頂点で足りるが、Radial の
//     塗り潰しはセルを扇形で切った凸多角形になる。180 度ごとに 2 区画へ割り、
//     各区画が最大 6 頂点 (= 4 三角形) まで太るので 24 頂点が上限になる。
//     借用制のバッファは 1 本の容量を後から変えられないため、最大側で確保する。
static constexpr uint32_t kImageVBVertices = 24;
static constexpr uint32_t kTextVBVertices  = 4096; // ~682 グリフ分。超過時は複数ドローに分割する

struct Rect { math::Vector2 pos; math::Vector2 size; };

int GetUISortOrder(GameObject& go)
{
    // UIImage と UIText を同じ GO に持つ Button は一つの描画単位として扱う。
    // 両方に値がある場合は手前側を採用し、どちらの Inspector からでも調整できるようにする。
    int order = 0;
    if (const auto* image = go.GetComponent<UIImage>()) order = image->sortOrder;
    if (const auto* text = go.GetComponent<UIText>()) order = (std::max)(order, text->sortOrder);
    return order;
}

// 子を sortOrder 順に out へ並べる。
//
// WHY 戻り値ではなく出力引数か:
//   この関数は UI ノード 1 つにつき毎フレーム 1 回呼ばれる。vector を返すと、
//   要素数ぶんの確保と解放が UI の規模 × フレームレートで走り続ける。
//   呼び出し側が持つ領域へ書けば、2 フレーム目以降の確保は 0 になる。
//
// WHY 呼び出し側で領域を使い回せるか:
//   この並びは再帰の子降下より前に確定し、子の処理中も読み続ける必要がある。
//   1 本の共有バッファでは再帰で踏み潰されるので、呼び出し側は深さごとに
//   別の領域を渡す (AcquireChildScratch を参照)。
void SortUIChildren(GameObject& go, std::vector<GameObject*>& out)
{
    out.clear();
    out.reserve(static_cast<size_t>(go.GetChildCount()));
    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i)) out.push_back(child);
    }
    // 同値時は Hierarchy 順を維持し、既存シーンの見た目を変えない。
    std::stable_sort(out.begin(), out.end(),
        [](GameObject* a, GameObject* b) {
            return GetUISortOrder(*a) < GetUISortOrder(*b);
        });
}

// 再帰の深さごとに 1 本ずつ領域を貸す。
// WHY 深さで分けるか: 子リストは子の再帰処理が終わるまで生きていないといけない。
//     1 本を共有すると、最初の子を降りた先で親のリストが上書きされ、
//     2 番目以降の兄弟が消える (深い階層ほど壊れ方が派手になる)。
//     深さは UI 階層の段数ぶんしか無く、一度伸びれば以降は確保が起きない。
//
// 返す参照は、より深い階層のためにここが再度呼ばれても生き続ける
// (UISystemContext::childScratch が deque である理由)。
std::vector<GameObject*>& AcquireChildScratch(UISystemContext& ctx, std::size_t depth)
{
    if (ctx.childScratch.size() <= depth)
        ctx.childScratch.resize(depth + 1);
    return ctx.childScratch[depth];
}

struct UITransform2D {
    math::Vector2 position = math::Vector2::ZERO;
    float rotationZ = 0.0f;
    // 親要素の矩形サイズ。アンカーはこれに対する割合なので、階層を降りるときに
    // 一緒に運ばないと「親のどこ」が決まらない。Canvas 直下では Canvas の寸法。
    math::Vector2 parentSize = math::Vector2::ZERO;
};

float ExtractZRotation(const math::Quaternion& q)
{
    return std::atan2f(2.0f * (q.w * q.z + q.x * q.y),
                       1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

math::Vector2 Rotate2D(const math::Vector2& v, float angle)
{
    const float c = std::cosf(angle);
    const float s = std::sinf(angle);
    return { v.x * c - v.y * s, v.x * s + v.y * c };
}

// @param parentSize 子から見た親の矩形サイズ。アンカーの基準になる。
UITransform2D ComposeUITransform(const UITransform2D& parent, const scene::Transform& local,
                                 const math::Vector2& parentSize)
{
    // WHY: UI の localScale.xy は「倍率」ではなく「幅・高さ」なので、
    //      通常の TransformSystem のように親 scale を子 position へ掛けると、
    //      親要素のサイズ変更だけで子が大きく飛んでしまう。
    // WHAT: 親の位置と回転は継承し、サイズは各 UI 要素自身の localScale.xy から読む。
    const math::Vector2 localPos = { local.position.x, local.position.y };
    UITransform2D result{};
    result.position = parent.position + Rotate2D(localPos, parent.rotationZ);
    result.rotationZ = parent.rotationZ + ExtractZRotation(local.rotation);
    // アンカー / ピボットの解釈はここではしない。矩形を決める 1 箇所
    // (ResolveUIRect) に閉じないと、位置の式が 2 つに分かれて必ず食い違う。
    // ここが運ぶのは「子から見た親のサイズ」だけ。
    result.parentSize = parentSize;
    return result;
}

// UI 要素の矩形。アンカー・ピボットの解釈は Components/UIRect.hpp が唯一の定義。
// WHY size を引数で受けるか: 画像は scale.xy がそのままサイズだが、文字は
//     実測しないと分からない。呼び出し側がそれぞれの方法で測ってから渡す。
Rect ResolveElementRect(const UITransform2D& resolved,
                        const math::Vector2& size,
                        const UIAnchor& anchoring)
{
    const UIRect rect = ResolveUIRect(resolved.parentSize,
                                      resolved.position, size, anchoring);
    return { rect.position, rect.size };
}

// UI 要素の矩形サイズ。
//
// WHY 1 本にまとめるか: 「この要素の大きさ」は矩形の解決・当たり判定・レイアウトの
//     3 箇所が必要とする。画像は transform.scale.xy がそのままサイズだが、文字は
//     実測 (UIText::resolvedSize) が正で、scale はその写しでしかない。
//     どこが写しでどこが正かを各所が個別に判断していると、文字を含む構成だけ
//     片方の経路がずれる。出所を選ぶのはここだけにする。
math::Vector2 UIElementSize(scene::GameObject& go)
{
    const math::Vector2 scaleSize = { go.transform.scale.x, go.transform.scale.y };
    if (go.GetComponent<UIImage>()) return scaleSize;
    if (const auto* text = go.GetComponent<UIText>()) {
        if (text->resolvedSize.x > 0.0f || text->resolvedSize.y > 0.0f)
            return text->resolvedSize;
    }
    return scaleSize;
}

UIAnchor UIElementAnchoring(scene::GameObject& go)
{
    if (const auto* image = go.GetComponent<UIImage>()) return image->anchoring;
    if (const auto* text = go.GetComponent<UIText>())   return text->anchoring;
    return {};
}

// GameObject の UI 矩形。持っているコンポーネントに応じてサイズの出所だけが変わり、
// アンカーの解き方は共通。
Rect RectFromTransform(scene::GameObject& go, const UITransform2D& resolved)
{
    return ResolveElementRect(resolved, UIElementSize(go), UIElementAnchoring(go));
}

// ── Matrix4 × Vector4 ────────────────────────────────────────────────────────
// WHY: Matrix4 に operator*(Vector4) がない場合も UISystem 内で使えるよう
//      インライン実装する（WorldSpace ヒット判定で使用）。
static math::Vector4 MulMV(const math::Matrix4& m, const math::Vector4& v)
{
    return {
        m.m[0][0]*v.x + m.m[0][1]*v.y + m.m[0][2]*v.z + m.m[0][3]*v.w,
        m.m[1][0]*v.x + m.m[1][1]*v.y + m.m[1][2]*v.z + m.m[1][3]*v.w,
        m.m[2][0]*v.x + m.m[2][1]*v.y + m.m[2][2]*v.z + m.m[2][3]*v.w,
        m.m[3][0]*v.x + m.m[3][1]*v.y + m.m[3][2]*v.z + m.m[3][3]*v.w,
    };
}

// ── UI マテリアル解決 ────────────────────────────────────────────────────────
//
// WHY メッシュ側の SyncMaterial を呼ばないか:
//   あちらは MaterialComponent (スロット / submesh / per-instance 上書き) を入口に持つ。
//   UI に submesh は無く、代わりに .mat を持たない要素が多数を占めるので、
//   同じ入口に載せると UIImage 全部へ MaterialComponent を要求することになる。
//   共有すべきなのは「入口」ではなく「.mat をシェーダーへ束縛する規則」で、
//   そちらは Engine/Asset/MaterialParamBinding.hpp に切り出して両者で使う。
UIMaterialBinding* ResolveUIMaterial(UISystemContext& ctx,
                                     renderer::ResourceManager& resources,
                                     const std::string& materialPath)
{
    if (materialPath.empty()) return nullptr;

    if (const auto it = ctx.materialCache.find(materialPath); it != ctx.materialCache.end())
        return it->second.valid ? &it->second : nullptr;

    // 失敗も含めてキャッシュへ入れる。読めない .mat を毎フレーム開き直すと、
    // 壊れた 1 件がフレーム時間を持っていくうえログが埋まる。
    UIMaterialBinding& binding = ctx.materialCache[materialPath];

    const auto assetHandle = asset::AssetManager::LoadMaterial(materialPath);
    const auto* matAsset = assetHandle.IsValid()
        ? asset::AssetManager::GetMaterial(assetHandle) : nullptr;
    if (!matAsset) {
        FBZZ_LOG_WARN("UI material load failed '%s' -> falling back to the built-in sprite shader.",
                      materialPath.c_str());
        return nullptr;
    }

    // WHY 用途を検査するか: メッシュ用の .mat は b0 を CameraConstants として読む。
    //     UI パスがそこへ ortho 行列を入れるので、割り当ててしまうと頂点が飛ぶか
    //     真っ黒になる。原因が絵からは絶対に分からない種類の事故なので名指しで止める。
    if (matAsset->renderPath != asset::RenderPath::UI) {
        FBZZ_LOG_WARN("UI material '%s' is not declared for UI (render_path must be \"ui\") "
                      "-> falling back to the built-in sprite shader.", materialPath.c_str());
        return nullptr;
    }
    if (matAsset->shaderPath.empty()) {
        FBZZ_LOG_WARN("UI material '%s' has no shader -> falling back to the built-in sprite shader.",
                      materialPath.c_str());
        return nullptr;
    }

    renderer::Material& material = binding.material;
    material.shaderPath = matAsset->shaderPath;
    material.shader     = resources.LoadShader(matAsset->shaderPath);
    if (!material.shader.IsValid()) {
        FBZZ_LOG_WARN("UI material '%s' shader '%s' failed to load -> falling back.",
                      materialPath.c_str(), matAsset->shaderPath.c_str());
        return nullptr;
    }

    // 記述子はここで値ごと持つ。シェーダーは ResourceManager がホットリロードで
    // 差し替えうるので、ポインタで持つと解決時の中身と食い違う瞬間ができる。
    if (auto* shader = resources.Get(material.shader))
        binding.descriptor = shader->GetDescriptor();

    material.paramData.assign(binding.descriptor.cbufferSize, 0u);
    if (binding.descriptor.IsValid()) {
        asset::InitDefaultMaterialParams(binding.descriptor, material.paramData);
        asset::ApplyMaterialAssetParams(*matAsset, binding.descriptor, material.paramData);
    }

    const auto texturePaths = asset::ResolveMaterialTexturePaths(*matAsset);
    material.textures.resize(texturePaths.size());
    for (size_t i = 0; i < texturePaths.size(); ++i) {
        material.textures[i] = texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : resources.LoadTexture(texturePaths[i]);
    }

    material.Init(resources, binding.descriptor.cbufferSize);
    material.Upload(resources, binding.descriptor);

    if (binding.descriptor.cbufferSize > 0) {
        // 上書きを持つ要素だけが使う。持たない要素は共有の paramsBuffer を直接読む。
        // 作業領域は解決時に確保しきる。描画中に伸ばすことは無い。
        binding.overrideScratch.resize(binding.descriptor.cbufferSize);
        binding.overrideConstants = resources.CreateConstantBuffer(binding.descriptor.cbufferSize);
    }

    // UI は常に深度を書かない。ScreenSpace は深度テストもしない、
    // WorldSpace は 3D の手前後関係に従う ─ 既定 PSO と同じ使い分けを blend だけ差し替える。
    binding.screenPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, matAsset->blendMode, renderer::DepthMode::DEPTH_OFF });
    binding.worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, matAsset->blendMode, renderer::DepthMode::DEPTH_READ });
    binding.valid    = true;
    return &binding;
}

// 要素ごとの上書きを DrawCall へ適用する。上書きが無ければ何もしない。
void ApplyUIMaterialOverrides(renderer::ResourceManager& resources,
                              UIMaterialBinding& binding,
                              const UIImage& image,
                              renderer::DrawCall& call)
{
    if (!image.materialTextureOverrides.empty()) {
        const auto& slotNames = asset::kMaterialTextureSlotNames;
        for (size_t slot = 0; slot < slotNames.size() && slot < call.textures.size(); ++slot) {
            const auto it = image.materialTextureOverrides.find(slotNames[slot]);
            if (it == image.materialTextureOverrides.end() || it->second.empty()) continue;
            if (const auto texture = resources.LoadTexture(it->second); texture.IsValid())
                call.textures[slot] = texture;
        }
    }

    if (image.materialParamOverrides.empty()) return;
    if (!binding.overrideConstants.IsValid()) return;
    if (binding.overrideScratch.size() != binding.material.paramData.size()) return;

    // 共有マテリアルの値を土台に、この要素ぶんだけ重ねて別の cbuffer へ流す。
    // 共有側の paramData は触らない ─ 触ると同じ .mat を使う他の要素へ波及する。
    // 長さは解決時から変わらないので、コピーで確保は起きない。
    std::memcpy(binding.overrideScratch.data(),
                binding.material.paramData.data(),
                binding.material.paramData.size());
    asset::ApplyMaterialParamOverrides(image.materialParamOverrides,
                                       binding.descriptor, binding.overrideScratch);
    resources.Update(binding.overrideConstants,
                     binding.overrideScratch.data(),
                     static_cast<uint32_t>(binding.overrideScratch.size()));
    call.constantBuffers[2] = binding.overrideConstants;
}

// ── UISystemContext 初期化 ────────────────────────────────────────────────────
void EnsureInit(UISystemContext& ctx, renderer::ResourceManager& resources)
{
    if (ctx.initialized) return;
    ctx.initialized = true;

    ctx.shader     = resources.LoadShader("Assets/shaders/UI/UISprite.hlsl");
    ctx.textShader = resources.LoadShader("Assets/shaders/UI/UIText.hlsl");
    ctx.constants  = resources.CreateConstantBuffer(sizeof(UIConstants));
    ctx.pso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    // WorldSpace / ScreenSpaceCamera UI: 深度テストあり・深度書き込みなし
    ctx.worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });

    static constexpr uint8_t kWhite[4] = { 255, 255, 255, 255 };
    ctx.whiteTexture = resources.CreateTexture(kWhite, 1, 1);

    if (!ctx.shader.IsValid() || !ctx.textShader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() || !ctx.whiteTexture.IsValid()) {
        FBZZ_LOG_ERROR("UISystem init failed: shader=%d textShader=%d cb=%d pso=%d worldPso=%d white=%d",
                       static_cast<int>(ctx.shader.IsValid()),
                       static_cast<int>(ctx.textShader.IsValid()),
                       static_cast<int>(ctx.constants.IsValid()),
                       static_cast<int>(ctx.pso.IsValid()),
                       static_cast<int>(ctx.worldPso.IsValid()),
                       static_cast<int>(ctx.whiteTexture.IsValid()));
    }
}

// この DrawCall 専用の頂点バッファを 1 本借りる。足りなければ増やし、以降は使い回す。
// WHY 借用制にするか: UISystemContext のコメントを参照。記録型バックエンドでは
//     「Draw ごとに別実体」が正しさの条件になる。
renderer::ResourceHandle<renderer::BufferTag> AcquireVertexBuffer(
    std::vector<renderer::ResourceHandle<renderer::BufferTag>>& pool,
    std::size_t& cursor,
    uint32_t vertexCapacity,
    renderer::ResourceManager& resources)
{
    if (cursor >= pool.size()) {
        pool.push_back(resources.CreateVertexBuffer(
            nullptr, vertexCapacity * sizeof(UIVertex), sizeof(UIVertex)));
    }
    return pool[cursor++];
}

// ── Canvas 収集 ──────────────────────────────────────────────────────────────
void CollectCanvasesRecursive(GameObject* go, std::vector<CanvasEntry>& canvases)
{
    if (!go || !go->activeInHierarchy()) return;
    if (auto* canvas = go->GetComponent<UICanvas>(); canvas && canvas->enabled)
        canvases.push_back({ go, canvas });
    for (int i = 0; i < go->GetChildCount(); ++i)
        CollectCanvasesRecursive(go->GetChild(i), canvases);
}

void CollectCanvases(Scene& scene, std::vector<CanvasEntry>& canvases)
{
    for (GameObject* root : scene.GetRootGameObjects()) {
        if (!root) continue;
        CollectCanvasesRecursive(root, canvases);
    }
    std::sort(canvases.begin(), canvases.end(),
        [](const CanvasEntry& a, const CanvasEntry& b) {
            return a.canvas->sortOrder < b.canvas->sortOrder;
        });
}

bool IsScreenSpaceRenderMode(UIRenderMode mode);

bool ShouldRenderCanvas(const UICanvas& canvas, UIRenderTargetView targetView)
{
    // WHY: GameViewport は最終出力として全 Canvas、SceneViewport は WorldSpace、
    //       CanvasEditor は ScreenSpace だけ描く。
    if (targetView == UIRenderTargetView::SceneViewport)
        return canvas.renderMode == UIRenderMode::WorldSpace;
    if (targetView == UIRenderTargetView::CanvasEditor)
        return IsScreenSpaceRenderMode(canvas.renderMode);
    return true;
}

bool IsScreenSpaceRenderMode(UIRenderMode mode)
{
    return mode == UIRenderMode::ScreenSpaceOverlay
        || mode == UIRenderMode::ScreenSpaceCamera;
}

float ResolveCanvasScale(const UICanvas& canvas, float viewportWidth, float viewportHeight)
{
    // WHY: Unity の Canvas Scaler と同じ考え方。基準解像度と現在 Viewport の差を
    //      UI 座標変換に集約する。
    if (canvas.scaleMode != UICanvasScaleMode::ScaleWithScreenSize)
        return 1.0f;

    const float refW = (std::max)(1.0f, canvas.referenceWidth);
    const float refH = (std::max)(1.0f, canvas.referenceHeight);
    const float scaleW = (std::max)(1.0f, viewportWidth) / refW;
    const float scaleH = (std::max)(1.0f, viewportHeight) / refH;
    const float match = std::clamp(canvas.matchWidthOrHeight, 0.0f, 1.0f);
    return std::exp(std::log(scaleW) * (1.0f - match) + std::log(scaleH) * match);
}

void ResolveScreenSpaceCanvasArea(const UICanvas& canvas,
                                  float viewportWidth,
                                  float viewportHeight,
                                  float& visibleCanvasW,
                                  float& visibleCanvasH)
{
    if (canvas.scaleMode == UICanvasScaleMode::ScaleWithScreenSize) {
        const float scale = ResolveCanvasScale(canvas, viewportWidth, viewportHeight);
        visibleCanvasW = (std::max)(1.0f, viewportWidth) / scale;
        visibleCanvasH = (std::max)(1.0f, viewportHeight) / scale;
        return;
    }
    visibleCanvasW = (std::max)(1.0f, canvas.canvasWidth);
    visibleCanvasH = (std::max)(1.0f, canvas.canvasHeight);
}

CanvasRuntimeState BuildCanvasRuntimeState(const UICanvas& canvas,
                                           const GameObject& canvasGO,
                                           float viewportWidth,
                                           float viewportHeight,
                                           math::Vector2 rawMouseInViewport,
                                           const math::Matrix4& viewProjection,
                                           math::Vector3 cameraWorldPos,
                                           math::Quaternion cameraWorldRot,
                                           const UISystemContext& ctx,
                                           UIRenderTargetView targetView)
{
    (void)targetView;
    CanvasRuntimeState state{};

    // ── WorldSpace ────────────────────────────────────────────────────────────
    if (canvas.renderMode == UIRenderMode::WorldSpace) {
        const float ws = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  ws;
        pixelToLocal.m[1][1] = -ws;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * ws;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * ws;

        // WHY: 再帰収集後は Canvas が子 GO に置かれる可能性があるため worldPosition を使う。
        //      faceCamera 時は向きをカメラ姿勢で上書きし、常にカメラへ正対させる (ビルボード)。
        //      位置は worldPosition のままなので、敵の頭上に置けば追従しつつ常に読める。
        const math::Quaternion canvasRotation =
            canvas.faceCamera ? cameraWorldRot : canvasGO.transform.worldRotation;
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasGO.transform.worldPosition,
            canvasRotation,
            math::Vector3::ONE
        );

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        state.mouseInCanvasSpace = rawMouseInViewport; // WorldSpace はレイキャストで処理
        state.pso   = ctx.worldPso;
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    // ── ScreenSpaceCamera ─────────────────────────────────────────────────────
    if (canvas.renderMode == UIRenderMode::ScreenSpaceCamera) {
        // Canvas をカメラ前方 planeDistance ワールド単位に配置し、カメラ向きで固定する。
        // WHY: Overlay と同じスクリーン UI に深度テストを加えたい場合に使う。
        //      マウス座標変換は Overlay と同じスクリーン座標系を維持する。
        const float ws = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  ws;
        pixelToLocal.m[1][1] = -ws;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * ws;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * ws;

        // カメラ前方ベクトル: worldRot * (0, 0, 1)
        const math::Vector3 camFwd = cameraWorldRot * math::Vector3{ 0.0f, 0.0f, 1.0f };
        const math::Vector3 canvasPos = cameraWorldPos + camFwd * canvas.planeDistance;
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasPos, cameraWorldRot, math::Vector3::ONE);

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        // マウス座標: Overlay と同じスクリーン→キャンバス変換
        float visibleW = 1.0f, visibleH = 1.0f;
        ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, visibleW, visibleH);
        const float scaleX = (std::max)(1.0f, viewportWidth) / visibleW;
        const float scaleY = (std::max)(1.0f, viewportHeight) / visibleH;
        state.mouseInCanvasSpace = { rawMouseInViewport.x / scaleX, rawMouseInViewport.y / scaleY };
        state.pso   = ctx.worldPso;  // 深度テストあり (3D オブジェクトに遮蔽可)
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    // ── ScreenSpaceOverlay (デフォルト) ───────────────────────────────────────
    float visibleCanvasW = 1.0f;
    float visibleCanvasH = 1.0f;
    ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight,
                                 visibleCanvasW, visibleCanvasH);

    const float scaleX = (std::max)(1.0f, viewportWidth) / visibleCanvasW;
    const float scaleY = (std::max)(1.0f, viewportHeight) / visibleCanvasH;

    state.canvasToClip = math::Matrix4::Orthographic(
        0.0f, visibleCanvasW,
        visibleCanvasH, 0.0f,
        0.0f, 1.0f);
    state.mouseInCanvasSpace = { rawMouseInViewport.x / scaleX, rawMouseInViewport.y / scaleY };
    state.pso   = ctx.pso;
    state.layer = renderer::RenderLayer::OVERLAY_LAYER;
    return state;
}

// ── UIButton 更新 ─────────────────────────────────────────────────────────────
bool UpdateButton(UIButton& button, const Rect& rect, math::Vector2 mouse, bool mousePressed)
{
    const UIButtonState previousState = button.state;
    button.onClick = false;
    button.onEnter = false;
    button.onExit  = false;

    if (!button.enabled || !button.isInteractable) {
        button.state            = UIButtonState::NORMAL;
        button.wasPressedOnThis = false;
        button.lastMouseState   = mousePressed;
        return false;
    }

    const bool hit               = mouse.x >= rect.pos.x && mouse.x <= rect.pos.x + rect.size.x
                                && mouse.y >= rect.pos.y && mouse.y <= rect.pos.y + rect.size.y;
    const bool mouseJustPressed  = mousePressed  && !button.lastMouseState;
    const bool mouseJustReleased = !mousePressed && button.lastMouseState;

    // WHY: ドラッグで流入した押下をクリックとして誤検出しないため。
    if (mouseJustPressed && hit)
        button.wasPressedOnThis = true;

    if (mouseJustReleased) {
        button.onClick          = button.wasPressedOnThis && hit;
        button.wasPressedOnThis = false;
    }

    button.lastMouseState = mousePressed;

    if (hit && mousePressed && button.wasPressedOnThis) {
        button.state = UIButtonState::PRESSED;
    } else if (hit) {
        button.state = UIButtonState::HOVERED;
    } else {
        button.state = UIButtonState::NORMAL;
    }

    button.onEnter = previousState == UIButtonState::NORMAL && button.state != UIButtonState::NORMAL;
    button.onExit  = previousState != UIButtonState::NORMAL && button.state == UIButtonState::NORMAL;
    return hit;
}

math::Vector4 ButtonTint(const UIButton& button)
{
    if (!button.enabled || !button.isInteractable) return button.disabledColor;
    if (button.state == UIButtonState::PRESSED) return button.pressedColor;
    if (button.state == UIButtonState::HOVERED) return button.hoverColor;
    return button.normalColor;
}

math::Vector4 ResolveButtonImageColor(const math::Vector4& imageColor, const UIButton& button)
{
    const math::Vector4 targetColor = ButtonTint(button);
    constexpr float MIN_COLOR_CHANNEL = 1.0e-5f;
    const auto relativeChannel = [=](float image, float normal, float target) {
        return normal > MIN_COLOR_CHANNEL ? image * (target / normal) : target;
    };

    // WHY: 単純な乗算ではUIImageとUIButtonの両方が暗色の場合にPressed Colorが黒へ潰れる。
    //      Normal Colorを基準に相対変換すれば、UIImage.color == normalColorの一般的な設定で
    //      Inspectorに指定したHover/Pressed Colorがそのまま画面へ反映される。
    return {
        relativeChannel(imageColor.x, button.normalColor.x, targetColor.x),
        relativeChannel(imageColor.y, button.normalColor.y, targetColor.y),
        relativeChannel(imageColor.z, button.normalColor.z, targetColor.z),
        relativeChannel(imageColor.w, button.normalColor.w, targetColor.w)
    };
}

// ── 描画サブミット ────────────────────────────────────────────────────────────
// 組み上がった三角形リストを 1 ドローとして積む。
//
// WHY 頂点を呼び出し側から受け取るか:
//   矩形しか描けない形にすると、Radial の塗り潰しのように「矩形を切った凸多角形」を
//   出す経路が別実装になり、マテリアル適用と定数バッファの組み立てが二重になる。
//   頂点の作り方だけを外へ出せば、以降の手順は 1 本で済む。
//
// vertices の uv は「このセルの中の 0..1」。アトラス上の位置は uvMin/uvMax が持つ。
void SubmitUIGeometry(renderer::IRenderer& renderer,
                      renderer::ResourceManager& resources,
                      UISystemContext& ctx,
                      const math::Matrix4& canvasToClip,
                      renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                      renderer::RenderLayer layer,
                      const UIVertex* vertices,
                      uint32_t vertexCount,
                      const math::Vector2& cellSize,
                      const math::Vector4& color,
                      const math::Vector2& uvMin,
                      const math::Vector2& uvMax,
                      renderer::ResourceHandle<renderer::TextureTag> texture,
                      UIMaterialBinding* material,
                      bool worldSpace,
                      const UIImage* overrideSource)
{
    if (vertexCount < 3 || vertexCount > kImageVBVertices) return;

    const auto vertexBuffer = AcquireVertexBuffer(
        ctx.imageVertexBuffers, ctx.imageVertexCursor, kImageVBVertices, resources);
    if (!vertexBuffer.IsValid()) return;
    resources.Update(vertexBuffer, vertices, vertexCount * sizeof(UIVertex));

    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = color;
    constants.uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
    // 9-slice やタイルはセルごとにここへ来るので、セルの寸法がそのまま渡る。
    // 角丸マテリアルを Sliced / Tiled と併用してはいけないのはこのため。
    constants.rect   = { cellSize.x, cellSize.y, 0.0f, 0.0f };
    resources.Update(ctx.constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    call.vertexBuffer       = vertexBuffer;
    call.shader             = ctx.shader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = ctx.constants;
    call.vertexCount        = vertexCount;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    call.textures[0]        = texture.IsValid() ? texture : ctx.whiteTexture;

    if (material && material->valid) {
        const renderer::Material& resolved = material->material;
        call.shader = resolved.shader;
        // .mat の blend_mode を焼いた PSO。作れていなければ既定の PSO のままにする。
        const auto materialPso = worldSpace ? material->worldPso : material->screenPso;
        if (materialPso.IsValid()) call.pipelineState = materialPso;
        // b2 = MaterialConstants。UICommon.hlsli が宣言するレジスタと 1 対 1。
        call.constantBuffers[2] = resolved.paramsBuffer;
        // WHY t0 を上書きするのは .mat がテクスチャを持つときだけか:
        //     図形をシェーダーで描く UI マテリアルは albedo を持たないことが多い。
        //     そこで無条件に上書きすると、UIImage 側で指定した絵が黙って消える。
        for (size_t slot = 0; slot < resolved.textures.size() && slot < call.textures.size(); ++slot) {
            if (resolved.textures[slot].IsValid())
                call.textures[slot] = resolved.textures[slot];
        }
        // 共有 .mat を適用したうえに、この要素だけの差分を重ねる。
        if (overrideSource)
            ApplyUIMaterialOverrides(resources, *material, *overrideSource, call);
    }
    renderer.Submit(call, resources);
}

// 矩形 1 枚。回転は中心まわり。
void SubmitImage(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources,
                 UISystemContext& ctx,
                 const math::Matrix4& canvasToClip,
                 renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                 renderer::RenderLayer layer,
                 const math::Vector2& position,
                 const math::Vector2& size,
                 const math::Vector4& color,
                 const math::Vector2& uvMin,
                 const math::Vector2& uvMax,
                 renderer::ResourceHandle<renderer::TextureTag> texture,
                 float zAngle = 0.0f,
                 UIMaterialBinding* material = nullptr,
                 bool worldSpace = false,
                 // 上書きの持ち主。マテリアルを使わない描画では nullptr。
                 const UIImage* overrideSource = nullptr)
{
    const float cx = position.x + size.x * 0.5f;
    const float cy = position.y + size.y * 0.5f;
    const float cosZ = std::cosf(zAngle);
    const float sinZ = std::sinf(zAngle);
    const float hW = size.x * 0.5f, hH = size.y * 0.5f;

    auto rot = [&](float lx, float ly) -> math::Vector2 {
        return { cx + lx * cosZ - ly * sinZ,
                 cy + lx * sinZ + ly * cosZ };
    };

    // WHY 頂点 UV を 0..1 にするか:
    //   以前はここでアトラス UV (uvMin..uvMax) を直接積み、頂点シェーダーが
    //   さらに g_UVRect で写像していた。二重写像なので uvRect が (0,0,1,1) の
    //   ときだけ正しく、スプライトの切り出し矩形を指定すると内側の一部しか
    //   サンプリングされなかった。矩形内の正規化座標を積んで写像を 1 回にする。
    //   ついでにこの 0..1 が、角丸などの図形を描くための矩形内座標になる。
    const UIVertex vertices[6] = {
        { rot(-hW, -hH), { 0.0f, 0.0f } },
        { rot(-hW,  hH), { 0.0f, 1.0f } },
        { rot( hW, -hH), { 1.0f, 0.0f } },
        { rot( hW, -hH), { 1.0f, 0.0f } },
        { rot(-hW,  hH), { 0.0f, 1.0f } },
        { rot( hW,  hH), { 1.0f, 1.0f } },
    };
    SubmitUIGeometry(renderer, resources, ctx, canvasToClip, pso, layer,
                     vertices, 6, size, color, uvMin, uvMax,
                     texture, material, worldSpace, overrideSource);
}

// ── 画像セル ──────────────────────────────────────────────────────────────────
// 9-slice もタイルも「元画像の一部を、要素の一部へ貼る」の繰り返しでしかない。
// セルという 1 単位へ揃えることで、塗り潰しのクリップを 1 か所に書けば
// Simple / Sliced / Tiled の全部へ同じように効く。
//
// pos は要素ローカル (左上原点・回転前)。回転は描画直前にまとめて掛ける。
struct ImageCell {
    math::Vector2 pos;
    math::Vector2 size;
    math::Vector2 uvMin;
    math::Vector2 uvMax;
};

// 1 要素を描くのに要る、セル間で変わらない値。
// WHY 束ねるか: セル単位の関数へ毎回 14 個の引数を並べると、順番違いが型で止まらない。
//
// NOTE: メンバー名 renderer が名前空間 renderer と重なるため、型は fbzz:: から書く。
struct ImageDrawContext {
    fbzz::renderer::IRenderer&       renderer;
    fbzz::renderer::ResourceManager& resources;
    UISystemContext&                 ctx;
    const math::Matrix4&             canvasToClip;
    fbzz::renderer::ResourceHandle<fbzz::renderer::PipelineStateTag> pso;
    fbzz::renderer::RenderLayer      layer;
    math::Vector2                    origin;      // 要素の左上 (Canvas 空間)
    math::Vector2                    elementSize;
    float                            cosZ;
    float                            sinZ;
    math::Vector4                    color;
    fbzz::renderer::ResourceHandle<fbzz::renderer::TextureTag> texture;
    UIMaterialBinding*               material;
    bool                             worldSpace;
    const UIImage*                   overrideSource;
};

// 要素ローカル座標を Canvas 座標へ。回転の中心は常に要素全体の中心。
//
// WHY セルの中心ではないか: セルごとに自分の中心で回すと、9-slice の四隅が
//     ばらばらに回って継ぎ目が開く。塗り潰しで削れたセルも同じで、
//     ゲージが減るたびに絵が動いてしまう。
math::Vector2 LocalToCanvas(const ImageDrawContext& draw, const math::Vector2& local)
{
    const float lx = local.x - draw.elementSize.x * 0.5f;
    const float ly = local.y - draw.elementSize.y * 0.5f;
    return { draw.origin.x + draw.elementSize.x * 0.5f + lx * draw.cosZ - ly * draw.sinZ,
             draw.origin.y + draw.elementSize.y * 0.5f + lx * draw.sinZ + ly * draw.cosZ };
}

void SubmitCell(const ImageDrawContext& draw, const ImageCell& cell)
{
    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) return;
    const math::Vector2 topLeft     = LocalToCanvas(draw, cell.pos);
    const math::Vector2 bottomLeft  = LocalToCanvas(draw, { cell.pos.x, cell.pos.y + cell.size.y });
    const math::Vector2 topRight    = LocalToCanvas(draw, { cell.pos.x + cell.size.x, cell.pos.y });
    const math::Vector2 bottomRight = LocalToCanvas(draw,
        { cell.pos.x + cell.size.x, cell.pos.y + cell.size.y });

    const UIVertex vertices[6] = {
        { topLeft,     { 0.0f, 0.0f } },
        { bottomLeft,  { 0.0f, 1.0f } },
        { topRight,    { 1.0f, 0.0f } },
        { topRight,    { 1.0f, 0.0f } },
        { bottomLeft,  { 0.0f, 1.0f } },
        { bottomRight, { 1.0f, 1.0f } },
    };
    SubmitUIGeometry(draw.renderer, draw.resources, draw.ctx, draw.canvasToClip,
                     draw.pso, draw.layer, vertices, 6, cell.size, draw.color,
                     cell.uvMin, cell.uvMax, draw.texture, draw.material,
                     draw.worldSpace, draw.overrideSource);
}

// 9-slice の 1 軸ぶんの境界 (4 本) と、それに対応する UV を作る。
//
// 元画像側 (UV を切る量) と描画側 (要素上の幅) を別々に詰めるのがこの関数の要点。
// multiplier で描画側だけを拡縮するため、両者は比例しない。
void BuildSliceAxis(float elementLength, float uvLow, float uvHigh, float texturePixels,
                    float borderLow, float borderHigh, float unitScale,
                    float* outEdge, float* outUv)
{
    const float sourceLength = (uvHigh - uvLow) * texturePixels;
    // 余白どうしが重なると中央が裏返る。まず元画像側で収める。
    const float sourceLow  = std::clamp(borderLow, 0.0f, sourceLength * 0.5f);
    const float sourceHigh = std::clamp(borderHigh, 0.0f, sourceLength - sourceLow);
    // 描画側は要素をはみ出せない。狭い要素では余白どうしが先に突き当たる。
    const float destLow  = std::min(sourceLow * unitScale, elementLength * 0.5f);
    const float destHigh = std::min(sourceHigh * unitScale, elementLength - destLow);

    outEdge[0] = 0.0f;
    outEdge[1] = destLow;
    outEdge[2] = elementLength - destHigh;
    outEdge[3] = elementLength;
    outUv[0] = uvLow;
    outUv[1] = uvLow + sourceLow / texturePixels;
    outUv[2] = uvHigh - sourceHigh / texturePixels;
    outUv[3] = uvHigh;
}

// 1 つの領域を step 間隔で敷き詰め、タイル 1 枚ずつを emit へ渡す。
// step が 0 以下の軸は分割しない (辺の帯を一方向だけタイルするのに使う)。
//
// WHY 枚数に上限を置くか: 1 タイルが数ピクセルの素材を全画面へ敷くと 1 要素で
//     数万ドローになる。上限に当たったらタイルを引き伸ばして枚数を抑える。
//     密度は指定どおりでなくなるが、隙間が空くよりは意図の伝わる絵になる。
constexpr int kMaxTilesPerAxis   = 128;
constexpr int kMaxTilesPerRegion = 4096;

template<typename EmitFn>
void EmitTiledRegion(const ImageCell& region, float stepX, float stepY, EmitFn& emit)
{
    if (region.size.x <= 0.0f || region.size.y <= 0.0f) return;

    const auto axisCount = [](float length, float step) {
        if (step <= 0.0f) return 1;
        return std::clamp(static_cast<int>(std::ceil(length / step)), 1, kMaxTilesPerAxis);
    };
    int countX = axisCount(region.size.x, stepX);
    int countY = axisCount(region.size.y, stepY);
    if (countX * countY > kMaxTilesPerRegion) {
        // 縦横の比を保ったまま総数を落とす。片方だけ削ると模様の目が伸びる。
        const float shrink = std::sqrt(
            static_cast<float>(countX * countY) / static_cast<float>(kMaxTilesPerRegion));
        countX = (std::max)(1, static_cast<int>(static_cast<float>(countX) / shrink));
        countY = (std::max)(1, static_cast<int>(static_cast<float>(countY) / shrink));
    }
    // 上限に当たった軸は「領域を count 等分した幅」まで広げる。当たっていなければ step のまま。
    const float spanX = (std::max)(stepX, region.size.x / static_cast<float>(countX));
    const float spanY = (std::max)(stepY, region.size.y / static_cast<float>(countY));

    const float du = region.uvMax.x - region.uvMin.x;
    const float dv = region.uvMax.y - region.uvMin.y;
    for (int row = 0; row < countY; ++row) {
        for (int column = 0; column < countX; ++column) {
            ImageCell tile;
            tile.pos = { region.pos.x + spanX * static_cast<float>(column),
                         region.pos.y + spanY * static_cast<float>(row) };
            // 端の 1 枚は途中で切れる。切れた分だけ UV も詰める。
            tile.size = {
                (std::min)(spanX, region.pos.x + region.size.x - tile.pos.x),
                (std::min)(spanY, region.pos.y + region.size.y - tile.pos.y)
            };
            if (tile.size.x <= 0.0f || tile.size.y <= 0.0f) continue;
            tile.uvMin = region.uvMin;
            tile.uvMax = { region.uvMin.x + du * (tile.size.x / spanX),
                           region.uvMin.y + dv * (tile.size.y / spanY) };
            emit(tile);
        }
    }
}

// imageType に応じてセルを列挙する。
//   Simple … 1 枚
//   Sliced … 最大 9 枚 (四隅は原寸、辺と中央は引き伸ばし)
//   Tiled  … Border があれば四隅は原寸のまま辺と中央をタイル、無ければ全面をタイル
template<typename EmitFn>
void ForEachImageCell(const UIImage& image, const math::Vector2& size,
                      const math::Vector2& uvMin, const math::Vector2& uvMax,
                      EmitFn&& emit)
{
    const math::Vector2 textureSize = image.resolvedTextureSize;
    const float unitScale = image.UnitScale();
    const bool hasTextureSize = textureSize.x > 0.0f && textureSize.y > 0.0f;
    const bool sliceable = image.imageType == UIImageType::Sliced
                        || image.imageType == UIImageType::Tiled;

    if (sliceable && hasTextureSize) {
        const math::Vector4 border = image.EffectiveBorder();
        // 余白が全部 0 なら格子を作る意味が無い。Sliced は 1 枚、Tiled は全面タイルへ落ちる。
        if (border.x > 0.0f || border.y > 0.0f || border.z > 0.0f || border.w > 0.0f) {
            float x[4], u[4], y[4], v[4];
            BuildSliceAxis(size.x, uvMin.x, uvMax.x, textureSize.x,
                           border.x, border.z, unitScale, x, u);
            BuildSliceAxis(size.y, uvMin.y, uvMax.y, textureSize.y,
                           border.y, border.w, unitScale, y, v);
            // 中央列 / 中央行の元画像での寸法。Tiled のタイル 1 枚の大きさになる。
            const float centerStepX = (u[2] - u[1]) * textureSize.x * unitScale;
            const float centerStepY = (v[2] - v[1]) * textureSize.y * unitScale;
            const bool tiled = image.imageType == UIImageType::Tiled;

            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    const ImageCell cell{
                        { x[column], y[row] },
                        { x[column + 1] - x[column], y[row + 1] - y[row] },
                        { u[column], v[row] },
                        { u[column + 1], v[row + 1] }
                    };
                    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) continue;
                    if (!tiled) { emit(cell); continue; }
                    // 四隅は原寸のまま。伸びる方向だけをタイルへ置き換える。
                    EmitTiledRegion(cell,
                                    column == 1 ? centerStepX : 0.0f,
                                    row    == 1 ? centerStepY : 0.0f,
                                    emit);
                }
            }
            return;
        }
    }

    if (image.imageType == UIImageType::Tiled) {
        const math::Vector2 tile = { image.resolvedSizePixels.x * unitScale,
                                     image.resolvedSizePixels.y * unitScale };
        if (tile.x > 0.0f && tile.y > 0.0f) {
            EmitTiledRegion(ImageCell{ { 0.0f, 0.0f }, size, uvMin, uvMax },
                            tile.x, tile.y, emit);
            return;
        }
    }

    emit(ImageCell{ { 0.0f, 0.0f }, size, uvMin, uvMax });
}

// 塗り潰し (Edge) でセルを削る。何も残らなければ false。
//
// 要素全体を 1 枚のセルとして渡せば、分割前と同じ「矩形と UV を端から削る」動きになる。
bool ClipCellToEdgeFill(ImageCell& cell, const math::Vector2& elementSize,
                        float amount, UIImageFillOrigin origin)
{
    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) return false;
    const float f = std::clamp(amount, 0.0f, 1.0f);

    float keepLeft = 0.0f, keepTop = 0.0f;
    float keepRight = elementSize.x, keepBottom = elementSize.y;
    switch (origin) {
    case UIImageFillOrigin::Left:   keepRight  = elementSize.x * f;          break;
    case UIImageFillOrigin::Right:  keepLeft   = elementSize.x * (1.0f - f); break;
    case UIImageFillOrigin::Bottom: keepTop    = elementSize.y * (1.0f - f); break;
    case UIImageFillOrigin::Top:    keepBottom = elementSize.y * f;          break;
    }

    const float x0 = std::max(cell.pos.x, keepLeft);
    const float x1 = std::min(cell.pos.x + cell.size.x, keepRight);
    const float y0 = std::max(cell.pos.y, keepTop);
    const float y1 = std::min(cell.pos.y + cell.size.y, keepBottom);
    if (x1 <= x0 || y1 <= y0) return false;

    const float du = cell.uvMax.x - cell.uvMin.x;
    const float dv = cell.uvMax.y - cell.uvMin.y;
    const math::Vector2 uvMin = {
        cell.uvMin.x + du * ((x0 - cell.pos.x) / cell.size.x),
        cell.uvMin.y + dv * ((y0 - cell.pos.y) / cell.size.y)
    };
    const math::Vector2 uvMax = {
        cell.uvMin.x + du * ((x1 - cell.pos.x) / cell.size.x),
        cell.uvMin.y + dv * ((y1 - cell.pos.y) / cell.size.y)
    };
    cell = ImageCell{ { x0, y0 }, { x1 - x0, y1 - y0 }, uvMin, uvMax };
    return true;
}

// ── Radial 塗り潰し ───────────────────────────────────────────────────────────
// 扇形は「中心を通る 2 つの半平面の共通部分」なので (180 度以下なら)、
// セルの四角形をその 2 枚で切れば残りがそのまま描く形になる。
// 三角形を並べて近似するのではなく切り取るため、どの角度でも輪郭が正確に出る。
struct ClipVertex {
    math::Vector2 local;
    math::Vector2 uv;   // セル内 0..1
};

// dot(p - point, normal) >= 0 側を残す (Sutherland-Hodgman)。
// 凸多角形の入力に対し、出力は入力 + 1 頂点までしか増えない。
int ClipHalfPlane(const ClipVertex* in, int count, const math::Vector2& point,
                  const math::Vector2& normal, ClipVertex* out)
{
    int outCount = 0;
    for (int i = 0; i < count; ++i) {
        const ClipVertex& a = in[i];
        const ClipVertex& b = in[(i + 1) % count];
        const float da = (a.local.x - point.x) * normal.x + (a.local.y - point.y) * normal.y;
        const float db = (b.local.x - point.x) * normal.x + (b.local.y - point.y) * normal.y;
        if (da >= 0.0f) out[outCount++] = a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            out[outCount++] = {
                { a.local.x + (b.local.x - a.local.x) * t,
                  a.local.y + (b.local.y - a.local.y) * t },
                { a.uv.x + (b.uv.x - a.uv.x) * t,
                  a.uv.y + (b.uv.y - a.uv.y) * t }
            };
        }
    }
    return outCount;
}

// fillOrigin が指す辺の方向を角度で返す。Canvas は y が下向きなので、
// 角度が増える向き = 画面上の時計回りになる。
float FillOriginAngle(UIImageFillOrigin origin)
{
    constexpr float kPi = 3.14159265358979323846f;
    switch (origin) {
    case UIImageFillOrigin::Right:  return 0.0f;
    case UIImageFillOrigin::Bottom: return kPi * 0.5f;
    case UIImageFillOrigin::Left:   return kPi;
    case UIImageFillOrigin::Top:    return -kPi * 0.5f;
    default: break;
    }
    return 0.0f;
}

// 扇形の定義。amount = 1 でちょうど要素全体を覆う位置と角度になる。
struct RadialSector {
    math::Vector2 apex{};        // 要素ローカルの回転軸
    float startAngle = 0.0f;     // fillAmount = 0 のときの縁
    float fullSweep  = 0.0f;     // amount = 1 での角度 (符号は回転方向)
};

// WHY 方式ごとに軸を動かすか:
//   90 / 180 は「四半円 / 半円で要素をちょうど覆う」形。中心を軸にしたまま
//   角度だけ 90 度に絞ると、覆えるのは要素の一部だけになって別物の絵になる。
//   軸を角 (90) や辺の中点 (180) へ移して初めて、その角度で全体が埋まる。
RadialSector BuildRadialSector(UIImageFillMethod method, UIImageFillOrigin origin,
                               bool clockwise, const math::Vector2& size)
{
    constexpr float kPi = 3.14159265358979323846f;
    const float direction = clockwise ? 1.0f : -1.0f;
    const UIImageFillOrigin resolved = NormalizeFillOrigin(method, origin);

    RadialSector sector;
    switch (method) {
    case UIImageFillMethod::Radial90: {
        // 角を軸に、そこから伸びる 2 辺の間を四半周する。
        struct Corner { math::Vector2 apex; float baseAngle; };
        const Corner corner = [&]() -> Corner {
            switch (resolved) {
            case UIImageFillOrigin::TopLeft:     return { { 0.0f,   0.0f   },  0.0f };
            case UIImageFillOrigin::TopRight:    return { { size.x, 0.0f   },  kPi * 0.5f };
            case UIImageFillOrigin::BottomRight: return { { size.x, size.y },  kPi };
            default:                             return { { 0.0f,   size.y }, -kPi * 0.5f };
            }
        }();
        sector.apex = corner.apex;
        // 覆う範囲は [baseAngle, baseAngle + 90 度]。どちら端から満ちるかだけが向きで変わる。
        sector.startAngle = clockwise ? corner.baseAngle : corner.baseAngle + kPi * 0.5f;
        sector.fullSweep  = direction * kPi * 0.5f;
        break;
    }
    case UIImageFillMethod::Radial180: {
        // 辺の中点を軸に、その辺に沿って半周する。内側へ向かう法線を必ず通る。
        struct Edge { math::Vector2 apex; float inwardAngle; };
        const Edge edge = [&]() -> Edge {
            switch (resolved) {
            case UIImageFillOrigin::Top:    return { { size.x * 0.5f, 0.0f   },  kPi * 0.5f };
            case UIImageFillOrigin::Left:   return { { 0.0f,   size.y * 0.5f },  0.0f };
            case UIImageFillOrigin::Right:  return { { size.x, size.y * 0.5f },  kPi };
            default:                        return { { size.x * 0.5f, size.y }, -kPi * 0.5f };
            }
        }();
        sector.apex = edge.apex;
        sector.startAngle = edge.inwardAngle - direction * kPi * 0.5f;
        sector.fullSweep  = direction * kPi;
        break;
    }
    case UIImageFillMethod::Radial360:
    default:
        sector.apex = { size.x * 0.5f, size.y * 0.5f };
        sector.startAngle = FillOriginAngle(resolved);
        sector.fullSweep  = direction * kPi * 2.0f;
        break;
    }
    return sector;
}

void SubmitCellRadial(const ImageDrawContext& draw, const ImageCell& cell,
                      float amount, const RadialSector& sector)
{
    constexpr float kPi = 3.14159265358979323846f;
    if (cell.size.x <= 0.0f || cell.size.y <= 0.0f) return;
    const float signedSweep = std::clamp(amount, 0.0f, 1.0f) * sector.fullSweep;
    const float sweep = std::fabs(signedSweep);
    if (sweep <= 0.0f) return;
    const bool clockwise = signedSweep > 0.0f;

    const ClipVertex polygon[4] = {
        { { cell.pos.x,                cell.pos.y                }, { 0.0f, 0.0f } },
        { { cell.pos.x,                cell.pos.y + cell.size.y  }, { 0.0f, 1.0f } },
        { { cell.pos.x + cell.size.x,  cell.pos.y + cell.size.y  }, { 1.0f, 1.0f } },
        { { cell.pos.x + cell.size.x,  cell.pos.y                }, { 1.0f, 0.0f } },
    };
    // セルが軸を含まなくても、半平面の切り取りは同じ式で効く。
    const math::Vector2 apex = sector.apex;
    const float base = sector.startAngle;
    // 180 度を超える扇形は 2 枚の半平面では表せない。等分して 2 区画に分ける。
    const int chunkCount = sweep > kPi ? 2 : 1;
    const float chunk = sweep / static_cast<float>(chunkCount);

    // WHY 境界角を先に配列へ出すか: 隣り合う区画は 1 本の境界を共有する。
    //     区画ごとに角度を足し引きして作ると丸め誤差で 2 本に割れ、境目に
    //     隙間か二重塗りの筋が出る。同じ値を両側から読ませて一致させる。
    float bounds[3] = {};
    for (int i = 0; i <= chunkCount; ++i) {
        const float offset = chunk * static_cast<float>(i);
        bounds[i] = clockwise ? base + offset : base - offset;
    }

    UIVertex vertices[kImageVBVertices];
    uint32_t count = 0;
    for (int i = 0; i < chunkCount; ++i) {
        // 切り取りは常に「角度の小さい側 → 大きい側」で渡す。
        const float low  = clockwise ? bounds[i]     : bounds[i + 1];
        const float high = clockwise ? bounds[i + 1] : bounds[i];
        const math::Vector2 dirLow  = { std::cosf(low),  std::sinf(low)  };
        const math::Vector2 dirHigh = { std::cosf(high), std::sinf(high) };

        // 内側の条件は cross(dirLow, v) >= 0 かつ cross(dirHigh, v) <= 0。
        // cross(d, v) は dot(v, (-d.y, d.x)) と同じなので、法線 2 本に直せる。
        ClipVertex work[10];
        ClipVertex clipped[10];
        int n = ClipHalfPlane(polygon, 4, apex, { -dirLow.y, dirLow.x }, work);
        n = ClipHalfPlane(work, n, apex, { dirHigh.y, -dirHigh.x }, clipped);

        for (int t = 1; t + 1 < n; ++t) {
            if (count + 3 > kImageVBVertices) break;
            vertices[count++] = { LocalToCanvas(draw, clipped[0].local),     clipped[0].uv };
            vertices[count++] = { LocalToCanvas(draw, clipped[t].local),     clipped[t].uv };
            vertices[count++] = { LocalToCanvas(draw, clipped[t + 1].local), clipped[t + 1].uv };
        }
    }
    if (count < 3) return;
    SubmitUIGeometry(draw.renderer, draw.resources, draw.ctx, draw.canvasToClip,
                     draw.pso, draw.layer, vertices, count, cell.size, draw.color,
                     cell.uvMin, cell.uvMax, draw.texture, draw.material,
                     draw.worldSpace, draw.overrideSource);
}

void SubmitRect(renderer::IRenderer& renderer,
                renderer::ResourceManager& resources,
                UISystemContext& ctx,
                const math::Matrix4& canvasToClip,
                renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                renderer::RenderLayer layer,
                math::Vector2 position,
                math::Vector2 size,
                const math::Vector4& color)
{
    SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                position, size, color,
                { 0.0f, 0.0f }, { 1.0f, 1.0f }, ctx.whiteTexture);
}

// ── フォントアトラス ──────────────────────────────────────────────────────────
// フォントの実体パスを解決する。
//
// WHY ここで解決するか:
//   FontAtlas は Renderer 層にあり、「プロジェクトの Assets → エンジン共有 Assets」の
//   二段構えを知らない。ファイルをそのまま fopen するだけなので、既定フォントを
//   自前で持たないプロジェクト (GreenWare の Assets/Fonts は空) では共有側の Roboto へ
//   辿り着けず、文字が 1 つも出ないまま終わる。
//
// WHY .fnt を付けてから解決するか:
//   FontAtlas は拡張子を自分で足す前提の「ベースパス」を受け取る。拡張子なしのまま
//   AssetManager へ渡すと実在チェックが必ず外れ、共有アセットへのフォールバックが働かない。
std::string ResolveFontBasePath(const std::string& basePath)
{
    if (basePath.empty()) return basePath;

    // .ttf / .otf / .ttc は動的モードで、パスがそのまま実体を指す。
    const std::size_t dot = basePath.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = basePath.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".ttf" || ext == ".ttc" || ext == ".otf")
            return asset::AssetManager::ResolveAssetPath(basePath);
    }

    const std::string fntExt = ".fnt";
    const std::string resolved = asset::AssetManager::ResolveAssetPath(basePath + fntExt);
    if (resolved.size() > fntExt.size() && resolved.ends_with(fntExt))
        return resolved.substr(0, resolved.size() - fntExt.size());
    return basePath;
}

renderer::FontAtlas& GetOrLoadFontAtlas(const std::string& basePath,
                                        UISystemContext& ctx,
                                        renderer::ResourceManager& resources)
{
    // キャッシュは指定されたパスで引く。解決結果でキーを作ると、同じ指定が
    // プロジェクト側と共有側で二重にロードされうる。
    auto it = ctx.fontAtlasCache.find(basePath);
    if (it != ctx.fontAtlasCache.end())
        return it->second;

    renderer::FontAtlas& atlas = ctx.fontAtlasCache[basePath];
    const std::string resolved = ResolveFontBasePath(basePath);
    if (!atlas.Load(resolved, resources)) {
        FBZZ_LOG_ERROR("UISystem: failed to load FontAtlas [%s]\n  resolved: %s",
                       basePath.c_str(), resolved.c_str());
    }

    return atlas;
}

// テキストを UTF-8 コードポイント単位で走査し、行ごとの表示幅 (スケール適用済み) を返す。
// 文字列を行へ分割し、各行のバイト範囲と幅を out へ書く (空文字列でも 1 行)。
//
// WHY: レイアウト計算 (ComputeTextLogicalSize) と描画 (SubmitTextWithAtlas) の両方が
//      まったく同じ送り幅の規則を必要とする。ここに 1 本化しないと、
//      カーニングや UTF-8 の扱いが片方だけズレて中央揃えが崩れる。
//      折り返しを入れるとさらに「どこで切れたか」まで一致している必要がある。
//
// WHY 幅の配列ではなくバイト範囲まで返すか: 折り返しは元の文字列に改行文字が
//      無い位置で行を切る。描く側が改行文字だけを見て走査していると、
//      測った行数と描く行数が食い違う。切れ目そのものを渡す。
void LayoutTextLines(const UIText& text,
                     const renderer::FontAtlas& atlas,
                     float scale,
                     std::vector<UITextLine>& out)
{
    out.clear();

    const float limit = text.maxWidth > 0.0f ? text.maxWidth : 0.0f;

    std::size_t lineBegin = 0;      // 今の行の先頭バイト
    float       lineW     = 0.0f;
    char32_t    previous  = 0;

    // 直近の折り返し候補 (空白の直後)。単語の途中で切らないために覚えておく。
    bool        hasBreak    = false;
    std::size_t breakEnd    = 0;    // candidate の行終端 (空白は含めない)
    std::size_t breakNext   = 0;    // 次の行の先頭
    float       breakWidth  = 0.0f;

    auto pushLine = [&](std::size_t end, float width) {
        out.push_back({ lineBegin, end, width });
    };

    std::size_t offset = 0;
    while (offset < text.text.size()) {
        const std::size_t charBegin = offset;
        const char32_t code = util::Utf8::Decode(text.text, offset);

        if (code == U'\n') {
            pushLine(charBegin, lineW);
            lineBegin = offset;
            lineW     = 0.0f;
            previous  = 0;
            hasBreak  = false;
            continue;
        }

        // カーニングは「前の文字との組」に対して定義されるため、行頭では適用しない。
        const float kerning = previous != 0 ? atlas.GetKerning(previous, code) * scale : 0.0f;
        const renderer::FontGlyph* glyph = atlas.GetGlyph(code);
        const float advance = (glyph ? glyph->advance : atlas.GetFallbackAdvance()) * scale
                            + text.letterSpacing;
        const float next = lineW + kerning + advance;

        // WHY 空白「の直後」を候補にするか: 行末に残った空白まで幅に数えると、
        //     右揃え・中央揃えのときに見えない余白のぶんだけ行がずれる。
        //     候補には空白を含めない幅を覚えておき、次の行は空白の後ろから始める。
        if (code == U' ' || code == U'\t') {
            hasBreak   = true;
            breakEnd   = charBegin;
            breakNext  = offset;
            breakWidth = lineW;
        }

        // 折り返し。行頭の 1 文字目は、はみ出しても切らない (切ると無限に進まない)。
        if (limit > 0.0f && next > limit && charBegin > lineBegin) {
            if (hasBreak && breakEnd > lineBegin) {
                pushLine(breakEnd, breakWidth);
                lineBegin = breakNext;
                offset    = breakNext;   // 空白の次から測り直す
            } else {
                // 空白の無い長い連なり (日本語・URL 等) は文字単位で折る。
                pushLine(charBegin, lineW);
                lineBegin = charBegin;
                offset    = charBegin;
            }
            lineW    = 0.0f;
            previous = 0;
            hasBreak = false;
            continue;
        }

        lineW    = next;
        previous = code;
    }
    pushLine(text.text.size(), lineW);
}

math::Vector2 ComputeTextLogicalSize(const UIText& text,
                                     UISystemContext& ctx,
                                     renderer::ResourceManager& resources)
{
    const std::string& path = text.fontPath.empty() ? ctx.defaultFontPath : text.fontPath;
    renderer::FontAtlas& atlas = GetOrLoadFontAtlas(path, ctx, resources);
    if (!atlas.IsValid()) return {};

    // 動的フォントでは、この文字列に必要なグリフをここで焼く。
    // WHY: レイアウトは送り幅を必要とするため、幅を測る前に登録が済んでいる必要がある。
    //      静的フォントでは即 return するのでコストはかからない。
    atlas.PrepareText(text.text, resources);

    const float scale = text.fontSize / atlas.GetLineHeight();
    const float cellH = atlas.GetLineHeight() * scale;

    LayoutTextLines(text, atlas, scale, ctx.textLineScratch);

    float maxW = 0.0f;
    for (const UITextLine& line : ctx.textLineScratch)
        maxW = (std::max)(maxW, line.width);

    // WHY 折り返し幅を指定したら箱の幅をそれにするか:
    //   指定した幅で折り返しているのに箱が「一番長い行」に縮むと、
    //   中央揃え・右揃えの基準が文言によって毎回変わる。指定した幅が箱。
    const float width = text.maxWidth > 0.0f ? text.maxWidth : maxW;
    return { width, cellH * static_cast<float>(ctx.textLineScratch.size()) };
}

void UpdateTextSizesRecursive(GameObject& go,
                              UISystemContext& ctx,
                              renderer::ResourceManager& resources)
{
    if (!go.activeInHierarchy()) return;
    if (auto* text = go.GetComponent<UIText>(); text && text->enabled && !text->text.empty()) {
        const math::Vector2 size = ComputeTextLogicalSize(*text, ctx, resources);
        // 実測サイズの置き場はここ。アンカー・ピボット・当たり判定・ギズモが読む。
        text->resolvedSize = size;
        // transform.scale へも書き戻す (後方互換の写し)。
        // NOTE: この行がある限り「文字の transform.scale はユーザーが設定できない」
        //       (毎フレーム上書きされる)。UISystem 内の読み手はすべて
        //       UIElementSize() 経由へ移したので、外しても矩形・当たり判定・
        //       レイアウトは動く。外していないのは、シーンやスクリプトが
        //       文字の scale を読んでいる可能性を実機で確認できていないため。
        go.transform.scale.x = size.x;
        go.transform.scale.y = size.y;
    }
    for (int i = 0; i < go.GetChildCount(); ++i)
        if (GameObject* child = go.GetChild(i))
            UpdateTextSizesRecursive(*child, ctx, resources);
}

void UITextSizeSystem(const std::vector<CanvasEntry>& canvases,
                      UISystemContext& ctx,
                      renderer::ResourceManager& resources)
{
    for (const CanvasEntry& entry : canvases)
        UpdateTextSizesRecursive(*entry.go, ctx, resources);
}

// ── テキスト描画（マルチドロー対応）─────────────────────────────────────────
void SubmitTextWithAtlas(renderer::IRenderer& renderer,
                         renderer::ResourceManager& resources,
                         UISystemContext& ctx,
                         const math::Matrix4& canvasToClip,
                         renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                         renderer::RenderLayer layer,
                         const UIText& text,
                         math::Vector2 position,
                         const math::Vector2& parentSize)
{
    renderer::FontAtlas& atlas = GetOrLoadFontAtlas(text.fontPath, ctx, resources);
    if (!atlas.IsValid()) return;

    // 描画経路からも焼いておく。
    // WHY: UITextSizeSystem を通らずに描画されるテキスト (サイズ自動調整の対象外) でも
    //      グリフが揃っている必要がある。登録済みなら UTF-8 走査だけで戻る。
    atlas.PrepareText(text.text, resources);

    const float scale = text.fontSize / atlas.GetLineHeight();
    const float cellH = atlas.GetLineHeight() * scale;

    // 整列と改行位置の決定に行分割が必要なので、常に事前計算する。
    // WHY: 旧実装は Left 以外のときだけ計算していたが、UTF-8 走査を 2 度書く方が
    //      ズレの温床になる。1 行あたり数十文字の走査であり、コストは無視できる。
    std::vector<UITextLine>& lines = ctx.textLineScratch;
    LayoutTextLines(text, atlas, scale, lines);

    // 実測サイズは UITextSizeSystem が同じフレームの描画前に確定させている。
    // WHY ここで測り直さないか: 同じ寸法を 2 箇所で出すと、片方だけ字送りや
    //     改行の扱いを直したときに「描画位置と当たり判定がずれる」が起きる。
    //     測るのは 1 箇所 (ComputeTextLogicalSize) だけにする。
    const math::Vector2 measured = (text.resolvedSize.x > 0.0f || text.resolvedSize.y > 0.0f)
        ? text.resolvedSize
        : math::Vector2{ 0.0f, cellH };

    // 矩形の左上。ここから先は画像とまったく同じ規則で置かれる。
    const UIRect box = ResolveUIRect(parentSize, position, measured, text.anchoring);

    // align は「確保した矩形の中で行をどちらへ寄せるか」だけを決める。
    // WHY 位置と切り離したか: 以前は align が position の意味まで変えていたので、
    //     中央揃えにした瞬間に文字が左へ半分ずれた。揃え方を変えただけで
    //     要素が動くのは、並べる作業のあいだずっと邪魔になる。
    auto lineStartX = [&](float lineWidth) -> float {
        if (text.align == TextAlign::Center) return box.position.x + (measured.x - lineWidth) * 0.5f;
        if (text.align == TextAlign::Right)  return box.position.x + (measured.x - lineWidth);
        return box.position.x;
    };

    // ページごとに頂点を分ける。
    // WHY: BMFont のマルチページアトラス (日本語のように 1 枚へ収まらない字種) では
    //      グリフごとに参照テクスチャが変わる。ページ単位でまとめてから
    //      ドローコールを分けることで、テクスチャ切り替えを最小回数に抑える。
    std::vector<std::vector<UIVertex2D>>& pageVerts = ctx.textPageScratch;
    if (pageVerts.size() < atlas.GetPageCount())
        pageVerts.resize(atlas.GetPageCount());
    if (pageVerts.empty()) return;
    const std::size_t pageCount = atlas.GetPageCount();
    for (std::size_t i = 0; i < pageCount; ++i) pageVerts[i].clear();
    pageVerts[0].reserve(text.text.size() * 6);

    // WHY 行ごとに走らせるか: 折り返しは元の文字列に改行文字が無い位置で行を切る。
    //     改行文字だけを頼りに 1 本で走ると、測った行数と描く行数が食い違う。
    math::Vector2 pen{};
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const UITextLine& line = lines[li];
        pen.x = lineStartX(line.width);
        pen.y = box.position.y + cellH * static_cast<float>(li);

        char32_t    previous = 0;
        std::size_t cursor   = line.begin;
        while (cursor < line.end) {
            const char32_t code = util::Utf8::Decode(text.text, cursor);
            if (code == U'\n') break;

            if (previous != 0)
                pen.x += atlas.GetKerning(previous, code) * scale;
            previous = code;

            const renderer::FontGlyph* g = atlas.GetGlyph(code);
            if (!g) {
                pen.x += atlas.GetFallbackAdvance() * scale + text.letterSpacing;
                continue;
            }

            // 空白文字のように画像を持たないグリフは、送りだけ進めて四角形を出さない。
            if (g->width > 0.0f && g->height > 0.0f) {
                const float x  = pen.x + g->xOffset * scale;
                const float y  = pen.y + g->yOffset * scale;
                const float x2 = x + g->width  * scale;
                const float y2 = y + g->height * scale;

                const std::size_t page =
                    (g->page >= 0 && static_cast<std::size_t>(g->page) < pageVerts.size())
                        ? static_cast<std::size_t>(g->page) : 0;
                std::vector<UIVertex2D>& verts = pageVerts[page];

                verts.push_back({ {x,  y},  { g->u0, g->v0 } });
                verts.push_back({ {x,  y2}, { g->u0, g->v1 } });
                verts.push_back({ {x2, y},  { g->u1, g->v0 } });
                verts.push_back({ {x2, y},  { g->u1, g->v0 } });
                verts.push_back({ {x,  y2}, { g->u0, g->v1 } });
                verts.push_back({ {x2, y2}, { g->u1, g->v1 } });
            }

            pen.x += g->advance * scale + text.letterSpacing;
        }
    }

    // 定数バッファは全ページ・全チャンク共通なので 1 回だけ更新する。
    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = text.color;
    constants.uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    // テキストは 1 ドローに複数グリフを詰めるため、矩形が 1 つに定まらない。
    // 図形を描くための寸法は無い、という意味でゼロを渡す。
    constants.rect   = { 0.0f, 0.0f, 0.0f, 0.0f };
    resources.Update(ctx.constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    // WHY: UIText.hlsl の .r チャンネルを coverage として使い、alpha チャンネル依存を排除する。
    call.shader             = ctx.textShader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = ctx.constants;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;

    // 未設定 (大多数) なら nullptr が返り、以降は組み込みのテキスト描画になる。
    if (const UIMaterialBinding* material =
            ResolveUIMaterial(ctx, resources, text.materialPath);
        material && material->valid) {
        const renderer::Material& resolved = material->material;
        call.shader = resolved.shader;
        const auto materialPso = (pso == ctx.worldPso) ? material->worldPso : material->screenPso;
        if (materialPso.IsValid()) call.pipelineState = materialPso;
        call.constantBuffers[2] = resolved.paramsBuffer;
        // WHY t0 を上書きしないか: t0 はフォントアトラスで、UISystem が
        //     ページごとに差し替えながら描く。マテリアルの albedo で潰すと
        //     文字が消える。追加の絵は t1 以降 (tex5-tex7) を使うこと。
        for (size_t slot = 1; slot < resolved.textures.size() && slot < call.textures.size(); ++slot) {
            if (resolved.textures[slot].IsValid())
                call.textures[slot] = resolved.textures[slot];
        }
    }

    for (std::size_t page = 0; page < pageVerts.size(); ++page) {
        const std::vector<UIVertex>& verts = pageVerts[page];
        if (verts.empty()) continue;

        call.textures[0] = atlas.GetTexture(static_cast<int>(page));
        if (!call.textures[0].IsValid()) continue;

        // kTextVBVertices を超えるテキストはチャンク分割して複数ドローコールで描く。
        // WHY: 固定 VB サイズを超えてもエラーで打ち切らず全グリフを描画するため。
        uint32_t offset = 0;
        const uint32_t total = static_cast<uint32_t>(verts.size());
        while (offset < total) {
            const uint32_t chunk = (std::min)(total - offset, kTextVBVertices);
            // チャンクごとに別実体を借りる。1 本を上書きすると、記録型バックエンドでは
            // 全チャンクが最後のグリフ群になる。
            const auto vertexBuffer = AcquireVertexBuffer(
                ctx.textVertexBuffers, ctx.textVertexCursor, kTextVBVertices, resources);
            if (!vertexBuffer.IsValid()) break;
            resources.Update(vertexBuffer, verts.data() + offset, chunk * sizeof(UIVertex));
            call.vertexBuffer = vertexBuffer;
            call.vertexCount  = chunk;
            renderer.Submit(call, resources);
            offset += chunk;
        }
    }
}

void SubmitText(renderer::IRenderer& renderer,
                renderer::ResourceManager& resources,
                UISystemContext& ctx,
                const math::Matrix4& canvasToClip,
                renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                renderer::RenderLayer layer,
                const UIText& text,
                math::Vector2 position,
                const math::Vector2& parentSize)
{
    if (!text.enabled || text.text.empty() || text.fontSize <= 0.0f) return;

    if (text.fontPath.empty()) {
        UIText defaulted = text;
        defaulted.fontPath = ctx.defaultFontPath;
        // resolvedSize は UITextSizeSystem が本体へ書き済み。複製は fontPath を
        // 補うだけなので、書き戻すものは何も無い。
        SubmitTextWithAtlas(renderer, resources, ctx, canvasToClip, pso, layer,
                            defaulted, position, parentSize);
        return;
    }
    SubmitTextWithAtlas(renderer, resources, ctx, canvasToClip, pso, layer,
                        text, position, parentSize);
}

// ── UILayoutGroup ─────────────────────────────────────────────────────────────
void ApplyLayout(GameObject& go, const UILayoutGroup& layout, float canvasScale,
                 UISystemContext& ctx)
{
    static std::unordered_set<const UILayoutGroup*> s_warnedLayouts;

    const int count = go.GetChildCount();

    // 毎フレーム・レイアウトグループごとに確保しないよう作業領域を借りる。
    std::vector<int>& indices = ctx.layoutIndexScratch;
    indices.clear();
    indices.reserve(count);
    for (int i = 0; i < count; ++i) {
        GameObject* child = go.GetChild(i);
        if (!child || !child->activeInHierarchy()) continue;
        indices.push_back(i);
    }
    if (layout.reverseOrder)
        std::reverse(indices.begin(), indices.end());

    float cursor = (layout.axis == UILayoutAxis::Horizontal) ? layout.paddingLeft : layout.paddingTop;
    for (int idx : indices) {
        GameObject* child = go.GetChild(idx);
        auto& t = child->transform;
        // 送り幅は transform.scale ではなく要素サイズから取る。
        // WHY: 文字の scale は実測値の写しでしかない。写しを読んでいると、
        //      「実測を scale へ書き戻す」処理を外した瞬間にレイアウトだけ崩れる。
        const math::Vector2 childSize = UIElementSize(*child);
        if (layout.axis == UILayoutAxis::Horizontal) {
            t.position.x = cursor;
            t.position.y = layout.paddingTop;
            cursor += childSize.x + layout.spacing;
        } else {
            t.position.x = layout.paddingLeft;
            t.position.y = cursor;
            cursor += childSize.y + layout.spacing;
        }
    }

    // paddingRight / paddingBottom がコンテナ末端として機能しているか検証する。
    // WHY: レイアウトはクリッピングを行わないためはみ出しは描画バグとして現れる。
    if (!indices.empty()) {
        const float endPos  = cursor - layout.spacing;
        const math::Vector2 containerSize = UIElementSize(go);
        const float contW   = containerSize.x;
        const float contH   = containerSize.y;
        const bool overflow = (layout.axis == UILayoutAxis::Horizontal)
            ? (endPos > contW - layout.paddingRight)
            : (endPos > contH - layout.paddingBottom);

        if (overflow && s_warnedLayouts.find(&layout) == s_warnedLayouts.end()) {
            s_warnedLayouts.insert(&layout);
            const float excess = (layout.axis == UILayoutAxis::Horizontal)
                ? (endPos - (contW - layout.paddingRight))
                : (endPos - (contH - layout.paddingBottom));
            FBZZ_LOG_WARN("UILayoutGroup: children overflow container by %.1f canvas-px (%.1f viewport-px, canvasScale=%.2f)",
                          excess, excess * canvasScale, canvasScale);
        }
    }
}

void ApplyUILayoutRecursive(GameObject& go, float canvasScale, UISystemContext& ctx)
{
    if (!go.activeInHierarchy()) return;

    if (auto* layout = go.GetComponent<UILayoutGroup>(); layout && layout->enabled)
        ApplyLayout(go, *layout, canvasScale, ctx);

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ApplyUILayoutRecursive(*child, canvasScale, ctx);
    }
}

// ── UIButton イベント処理 ─────────────────────────────────────────────────────
bool ProcessUIEventsRecursive(GameObject& go,
                              UISystemContext& ctx,
                              const UITransform2D& parentTransform,
                              math::Vector2 mouseInCanvasSpace,
                              bool mousePressed,
                              bool inputAvailable = true,
                              bool applySelfTransform = true,
                              std::size_t depth = 0)
{
    if (!go.activeInHierarchy()) return false;

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;

    const Rect selfRect = RectFromTransform(go, resolved);
    const bool insideSelf = mouseInCanvasSpace.x >= selfRect.pos.x
        && mouseInCanvasSpace.x <= selfRect.pos.x + selfRect.size.x
        && mouseInCanvasSpace.y >= selfRect.pos.y
        && mouseInCanvasSpace.y <= selfRect.pos.y + selfRect.size.y;
    bool childrenInputAvailable = inputAvailable;
    if (const auto* mask = go.GetComponent<UIMask>();
        mask && mask->enabled && mask->affectChildren && !insideSelf)
        childrenInputAvailable = false;
    UITransform2D childTransform = resolved;
    // 子のアンカーは「この要素の矩形」に対する割合になる。
    childTransform.parentSize = selfRect.size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= Rotate2D(scroll->scrollPosition, resolved.rotationZ);

    // 描画順の逆から入力を解決し、重なった UI では最前面の要素だけがポインターを受け取る。
    bool consumed = false;
    std::vector<GameObject*>& children = AcquireChildScratch(ctx, depth);
    SortUIChildren(go, children);
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        consumed |= ProcessUIEventsRecursive(**it, ctx, childTransform, mouseInCanvasSpace,
                                             mousePressed, childrenInputAvailable && !consumed,
                                             true, depth + 1);
    }

    auto* button = go.GetComponent<UIButton>();
    if (button) {
        // UIImage の有無に関わらず、解決した矩形に面積があればヒット判定する。
        // WHY: UIImage なしで UIText / 子要素だけで構成されるボタンにも対応する。
        // WHY transform.scale を直接見ないか: 判定に使う矩形は selfRect であって
        //     scale ではない。別の値で「押せるか」を決めていると、アンカーや
        //     実測サイズが絡んだ構成で「見えているのに押せない」がまれに出る。
        if (selfRect.size.x > 0.0f && selfRect.size.y > 0.0f) {
            const math::Vector2 effectiveMouse = inputAvailable && !consumed
                ? mouseInCanvasSpace
                : math::Vector2{ -1.0e30f, -1.0e30f };
            consumed |= UpdateButton(*button, selfRect, effectiveMouse, mousePressed);
        }
        else {
            button->onClick          = false;
            button->onEnter          = false;
            button->onExit           = false;
            button->state            = UIButtonState::NORMAL;
            button->wasPressedOnThis = false;
            button->lastMouseState   = mousePressed;
        }
    }

    const Rect rect = RectFromTransform(go, resolved);
    const bool hit = mouseInCanvasSpace.x >= rect.pos.x
        && mouseInCanvasSpace.x <= rect.pos.x + rect.size.x
        && mouseInCanvasSpace.y >= rect.pos.y
        && mouseInCanvasSpace.y <= rect.pos.y + rect.size.y;
    const bool canReceive = inputAvailable && !consumed;

    if (auto* slider = go.GetComponent<UISlider>()) {
        slider->onValueChanged = false;
        const bool justPressed = mousePressed && !slider->runtimeLastMouse;
        if (slider->enabled && slider->interactable && canReceive && justPressed && hit)
            slider->runtimeDragging = true;
        if (!mousePressed)
            slider->runtimeDragging = false;
        if (slider->runtimeDragging) {
            const float axisSize = slider->vertical ? rect.size.y : rect.size.x;
            const float axisPosition = slider->vertical
                ? mouseInCanvasSpace.y - rect.pos.y
                : mouseInCanvasSpace.x - rect.pos.x;
            float normalized = axisSize > 0.0f
                ? std::clamp(axisPosition / axisSize, 0.0f, 1.0f) : 0.0f;
            if (slider->vertical)
                normalized = 1.0f - normalized;
            float next = slider->minimum + (slider->maximum - slider->minimum) * normalized;
            if (slider->wholeNumbers)
                next = std::round(next);
            next = std::clamp(next, (std::min)(slider->minimum, slider->maximum),
                              (std::max)(slider->minimum, slider->maximum));
            slider->onValueChanged = next != slider->value;
            slider->value = next;
            if (auto* sliderImage = go.GetComponent<UIImage>()) {
                const float range = slider->maximum - slider->minimum;
                sliderImage->fillAmount = std::abs(range) > 0.000001f
                    ? std::clamp((slider->value - slider->minimum) / range, 0.0f, 1.0f)
                    : 0.0f;
                sliderImage->fillOrigin = slider->vertical
                    ? UIImageFillOrigin::Bottom : UIImageFillOrigin::Left;
            }
            consumed = true;
        }
        slider->runtimeLastMouse = mousePressed;
    }

    if (auto* toggle = go.GetComponent<UIToggle>()) {
        toggle->onValueChanged = false;
        const bool justPressed = mousePressed && !toggle->runtimeLastMouse;
        const bool justReleased = !mousePressed && toggle->runtimeLastMouse;
        if (toggle->enabled && toggle->interactable && canReceive && justPressed && hit)
            toggle->runtimePressedHere = true;
        if (justReleased) {
            if (toggle->runtimePressedHere && hit) {
                toggle->isOn = !toggle->isOn;
                toggle->onValueChanged = true;
                consumed = true;
            }
            toggle->runtimePressedHere = false;
        }
        toggle->runtimeLastMouse = mousePressed;
    }

    if (auto* scroll = go.GetComponent<UIScrollView>()) {
        scroll->onValueChanged = false;
        const float wheel = input::Input::MouseScrollDelta();
        if (scroll->enabled && canReceive && hit && wheel != 0.0f) {
            if (scroll->vertical)
                scroll->scrollPosition.y -= wheel * scroll->sensitivity;
            if (scroll->horizontal)
                scroll->scrollPosition.x -= wheel * scroll->sensitivity;
            scroll->scrollPosition.x = std::clamp(scroll->scrollPosition.x, 0.0f,
                (std::max)(0.0f, scroll->contentSize.x - rect.size.x));
            scroll->scrollPosition.y = std::clamp(scroll->scrollPosition.y, 0.0f,
                (std::max)(0.0f, scroll->contentSize.y - rect.size.y));
            scroll->onValueChanged = true;
            consumed = true;
        }
    }

    if (auto* field = go.GetComponent<UIInputField>()) {
        field->onValueChanged = false;
        field->onSubmit = false;
        if (mousePressed && canReceive)
            field->focused = hit;
        if (field->enabled && field->interactable && field->focused) {
            for (const char character : input::Input::TextInput()) {
                if (character == '\b') {
                    if (!field->text.empty()) {
                        size_t characterStart = field->text.size() - 1;
                        while (characterStart > 0
                               && (static_cast<unsigned char>(field->text[characterStart]) & 0xC0u) == 0x80u)
                            --characterStart;
                        field->text.erase(characterStart);
                        field->onValueChanged = true;
                    }
                } else if (character == '\r' || character == '\n') {
                    if (field->multiline) {
                        field->text.push_back('\n');
                        field->onValueChanged = true;
                    } else {
                        field->onSubmit = true;
                        field->focused = false;
                    }
                } else if ((field->contentType == UIInputContentType::Standard
                            || field->contentType == UIInputContentType::Password
                            || (field->contentType == UIInputContentType::Integer
                                && (character >= '0' && character <= '9'
                                    || character == '-' && field->text.empty()))
                            || (field->contentType == UIInputContentType::Decimal
                                && (character >= '0' && character <= '9'
                                    || character == '-' && field->text.empty()
                                    || character == '.' && field->text.find('.') == std::string::npos)))
                           && (field->characterLimit <= 0
                               || static_cast<int>(field->text.size()) < field->characterLimit)) {
                    field->text.push_back(character);
                    field->onValueChanged = true;
                }
            }
            field->caretPosition = field->text.size();
            consumed |= hit;
        }
        if (auto* inputText = go.GetComponent<UIText>()) {
            if (field->text.empty()) {
                inputText->text = field->placeholder;
            } else if (field->contentType == UIInputContentType::Password) {
                inputText->text.assign(field->text.size(), '*');
            } else {
                inputText->text = field->text;
            }
        }
    }

    if (auto* trigger = go.GetComponent<UIEventTrigger>()) {
        trigger->pointerEnter = trigger->pointerExit = false;
        trigger->pointerDown = trigger->pointerUp = trigger->pointerClick = false;
        trigger->beginDrag = trigger->drag = trigger->endDrag = false;
        trigger->pointerPosition = mouseInCanvasSpace;
        trigger->dragDelta = mouseInCanvasSpace - trigger->runtimeLastPointer;
        const bool currentHit = trigger->enabled && canReceive && hit;
        trigger->pointerEnter = currentHit && !trigger->runtimeHovered;
        trigger->pointerExit = !currentHit && trigger->runtimeHovered;
        trigger->pointerDown = currentHit && mousePressed && !trigger->runtimeLastMouse;
        trigger->pointerUp = !mousePressed && trigger->runtimeLastMouse
            && trigger->runtimePressedHere;
        if (trigger->pointerDown) {
            trigger->runtimePressedHere = true;
            trigger->beginDrag = true;
        }
        trigger->drag = trigger->runtimePressedHere && mousePressed
            && trigger->dragDelta != math::Vector2::ZERO;
        if (trigger->pointerUp) {
            trigger->pointerClick = currentHit;
            trigger->endDrag = true;
            trigger->runtimePressedHere = false;
        }
        trigger->runtimeHovered = currentHit;
        trigger->runtimeLastMouse = mousePressed;
        trigger->runtimeLastPointer = mouseInCanvasSpace;
        consumed |= trigger->pointerDown || trigger->drag;
    }
    return consumed;
}

// UIButton の状態別スプライトを、同じ GameObject の UIImage へ反映する。
//
// WHY 描画側で当てるか: 差し替えは「今どの状態か」の関数でしかなく、状態は
//     入力処理が毎フレーム更新する。イベント側で書くと、状態が変わらなかった
//     フレームや Play していない Editor で絵が付かない経路ができる。
void ApplyButtonSpriteSwap(UIImage& image, UIButton* button)
{
    // 空欄の状態は差し替えない = UIImage に指定された絵のまま。
    const std::string* next = nullptr;
    if (button != nullptr) {
        if (!button->enabled || !button->isInteractable) {
            if (!button->disabledSprite.empty())    next = &button->disabledSprite;
            else if (!button->normalSprite.empty()) next = &button->normalSprite;
        } else if (button->state == UIButtonState::PRESSED && !button->pressedSprite.empty()) {
            next = &button->pressedSprite;
        } else if (button->state == UIButtonState::HOVERED && !button->hoveredSprite.empty()) {
            next = &button->hoveredSprite;
        } else if (!button->normalSprite.empty()) {
            next = &button->normalSprite;
        }
    }

    if (next == nullptr) {
        image.hasTextureOverride = false;
        image.overrideTexturePath.clear();
        return;
    }
    image.hasTextureOverride = true;
    if (image.overrideTexturePath != *next) image.overrideTexturePath = *next;
}

// texturePath から GPU テクスチャと、Sprite サブアセットの切り出しを解決する。
//
// WHY 解決できるまで持ち越すか: テクスチャは非同期に読み込まれるので、要求した
//     フレームには寸法が無い。寸法が無いと UV も Border もタイル寸法も出せない。
//     1 回きりの解決にすると、その「まだ無い」状態が固定されて絵が伸びたままになる。
void ResolveImageTexture(UIImage& image, renderer::ResourceManager& resources)
{
    const std::string& source = image.EffectiveTexturePath();
    if (source.empty()) {
        if (image.loadedTexturePath.empty()) return;
        image.texture = {};
        image.loadedTexturePath.clear();
        image.hasResolvedSprite = false;
        image.resolvedSpriteUvMin = { 0.0f, 0.0f };
        image.resolvedSpriteUvMax = { 1.0f, 1.0f };
        image.resolvedSpriteBorder = {};
        image.resolvedTextureSize = {};
        image.resolvedSizePixels = {};
        return;
    }
    if (source == image.loadedTexturePath) return;

    std::string texturePath;
    std::string spriteId;
    (void)asset::ParseSpriteReference(source, texturePath, spriteId);
    image.texture = resources.LoadTexture(texturePath);

    const renderer::ITexture* texture = resources.Get(image.texture);
    // ハンドルは取れたが実体がまだ無い = 読み込み中。次のフレームで解決し直す。
    // 無効ハンドル (存在しないパス) は待っても変わらないので、ここで確定させる。
    if (image.texture.IsValid() && texture == nullptr) return;

    const float width  = texture ? static_cast<float>(texture->GetWidth())  : 0.0f;
    const float height = texture ? static_cast<float>(texture->GetHeight()) : 0.0f;
    const asset::ResolvedSprite resolved =
        asset::ResolveSpriteReference(source, width, height);

    image.resolvedTextureSize  = { width, height };
    image.resolvedSizePixels   = resolved.sizePixels;
    image.resolvedSpriteUvMin  = resolved.uvMin;
    image.resolvedSpriteUvMax  = resolved.uvMax;
    image.resolvedSpriteBorder = resolved.border;
    image.hasResolvedSprite    = resolved.resolved;
    image.loadedTexturePath    = source;
}

// ── デバッグ表示 ─────────────────────────────────────────────────────────────
// 矩形・アンカー・ピボットを白テクスチャの細い四角で重ねる。
//
// WHY 判定に使っている値をそのまま描くか: 別に計算した図を出すと、図と実際の
//     判定がずれたときに「表示のバグ」なのか「判定のバグ」なのかが分からない。
//     RectFromTransform が返した矩形そのものを描く。
void SubmitDebugRect(renderer::IRenderer& renderer,
                     renderer::ResourceManager& resources,
                     UISystemContext& ctx,
                     const math::Matrix4& canvasToClip,
                     renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                     renderer::RenderLayer layer,
                     const math::Vector2& position,
                     const math::Vector2& size,
                     const math::Vector4& color,
                     float thickness)
{
    const math::Vector2 uvMin{ 0.0f, 0.0f };
    const math::Vector2 uvMax{ 1.0f, 1.0f };
    auto bar = [&](float x, float y, float w, float h) {
        if (w <= 0.0f || h <= 0.0f) return;
        SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                    { x, y }, { w, h }, color, uvMin, uvMax, ctx.whiteTexture);
    };
    bar(position.x, position.y, size.x, thickness);                      // 上
    bar(position.x, position.y + size.y - thickness, size.x, thickness); // 下
    bar(position.x, position.y, thickness, size.y);                      // 左
    bar(position.x + size.x - thickness, position.y, thickness, size.y); // 右
}

void SubmitDebugMarker(renderer::IRenderer& renderer,
                       renderer::ResourceManager& resources,
                       UISystemContext& ctx,
                       const math::Matrix4& canvasToClip,
                       renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                       renderer::RenderLayer layer,
                       const math::Vector2& point,
                       const math::Vector4& color,
                       float radius)
{
    SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                { point.x - radius, point.y - radius }, { radius * 2.0f, radius * 2.0f },
                color, { 0.0f, 0.0f }, { 1.0f, 1.0f }, ctx.whiteTexture);
}

// ── レンダリング ──────────────────────────────────────────────────────────────
void RenderCanvasRecursive(GameObject& go,
                           const UITransform2D& parentTransform,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           UISystemContext& ctx,
                           const math::Matrix4& canvasToClip,
                           renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                           renderer::RenderLayer layer,
                           bool applySelfTransform = true,
                           std::size_t depth = 0)
{
    if (!go.activeInHierarchy()) return;

    UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;
    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    auto* text   = go.GetComponent<UIText>();

    // WHAT: 押下中はボタン全体を右下へ沈ませ、物理ボタンのような視覚フィードバックを出す。
    // WHY: pressedColorだけでは暗い背景画像との乗算後に差が小さくなり、入力がUIButtonまで
    //      届いたのか、Scene遷移スクリプトで止まったのかを画面上で判別できないため。
    if (button && button->enabled && button->isInteractable
        && button->state == UIButtonState::PRESSED) {
        constexpr math::Vector2 PRESSED_VISUAL_OFFSET = { 2.0f, 2.0f };
        resolved.position += Rotate2D(PRESSED_VISUAL_OFFSET, resolved.rotationZ);
    }

    if (image && image->enabled) {
        ApplyButtonSpriteSwap(*image, button);
        ResolveImageTexture(*image, resources);

        const Rect    r     = RectFromTransform(go, resolved);
        math::Vector4 color = image->color;
        if (button)
            color = ResolveButtonImageColor(color, *button);

        const math::Vector2 uvMin = image->hasResolvedSprite
            ? image->resolvedSpriteUvMin : image->uvMin;
        const math::Vector2 uvMax = image->hasResolvedSprite
            ? image->resolvedSpriteUvMax : image->uvMax;
        const float fillAmount = std::clamp(image->fillAmount, 0.0f, 1.0f);

        if (r.size.x > 0.0f && r.size.y > 0.0f && fillAmount > 0.0f) {
            // 未設定 (圧倒的多数) なら nullptr が返り、以降は従来どおりの経路になる。
            UIMaterialBinding* material =
                ResolveUIMaterial(ctx, resources, image->materialPath);
            const ImageDrawContext draw{
                renderer, resources, ctx, canvasToClip, pso, layer,
                r.pos, r.size,
                std::cosf(resolved.rotationZ), std::sinf(resolved.rotationZ),
                color,
                image->texture.IsValid() ? image->texture : ctx.whiteTexture,
                material,
                // WorldSpace / ScreenSpaceCamera Canvas は深度テストありの PSO を使う。
                // マテリアル側にも同じ使い分けの PSO があるので、どちらかを選ぶ。
                (pso == ctx.worldPso),
                image
            };

            // 塗り潰しは描画時にセルを削るだけで、transform.scale もレイアウトも
            // 当たり判定も動かさない (ゲージが減っても押せる範囲は変わらない)。
            const bool radial = image->fillMethod != UIImageFillMethod::Edge;
            const RadialSector sector = radial
                ? BuildRadialSector(image->fillMethod, image->fillOrigin,
                                    image->fillClockwise, r.size)
                : RadialSector{};
            const UIImageFillOrigin edgeOrigin =
                NormalizeFillOrigin(UIImageFillMethod::Edge, image->fillOrigin);
            ForEachImageCell(*image, r.size, uvMin, uvMax,
                [&](const ImageCell& cell) {
                    if (fillAmount >= 1.0f) {
                        SubmitCell(draw, cell);
                    } else if (radial) {
                        SubmitCellRadial(draw, cell, fillAmount, sector);
                    } else {
                        ImageCell clipped = cell;
                        if (ClipCellToEdgeFill(clipped, r.size, fillAmount, edgeOrigin))
                            SubmitCell(draw, clipped);
                    }
                });
        }
    }

    if (text && text->enabled) {
        SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, *text,
                   resolved.position, resolved.parentSize);
    }

    // 判定に使っているのと同じ矩形を重ねる。図と判定を別に計算しない。
    if (ctx.showRects) {
        const Rect debugRect = RectFromTransform(go, resolved);
        if (debugRect.size.x > 0.0f && debugRect.size.y > 0.0f) {
            const UIAnchor anchoring = UIElementAnchoring(go);
            SubmitDebugRect(renderer, resources, ctx, canvasToClip, pso, layer,
                            debugRect.pos, debugRect.size,
                            { 0.0f, 0.85f, 1.0f, 0.55f }, 1.0f);
            // ピボット (自分のどこが基準点に合っているか)。Editor のギズモと同じ桃色。
            //
            // WHY 実行時はアンカーを描かないか: アンカーは「親の矩形の中の点」で、
            //     意味を持たせるには親の矩形も一緒に見えている必要がある。
            //     並べる作業をするのは編集中なので、親子関係まで出すのは
            //     選択中の要素に絞れる Editor のギズモの仕事にする。
            //     実行時に知りたいのは「判定に使われている矩形はどこか」の一点。
            SubmitDebugMarker(renderer, resources, ctx, canvasToClip, pso, layer,
                              { debugRect.pos.x + debugRect.size.x * anchoring.pivot.x,
                                debugRect.pos.y + debugRect.size.y * anchoring.pivot.y },
                              { 1.0f, 0.47f, 0.78f, 0.9f }, 3.0f);
        }
    }

    UITransform2D childTransform = resolved;
    // 子のアンカーは「この要素の矩形」に対する割合になる。
    childTransform.parentSize = RectFromTransform(go, resolved).size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= Rotate2D(scroll->scrollPosition, resolved.rotationZ);
    // WHY 参照で受けるか: この並びは子の再帰処理が終わるまで読み続けるので、
    //     深さごとの領域を借りたまま降りる。コピーすると確保が戻ってくる。
    std::vector<GameObject*>& children = AcquireChildScratch(ctx, depth);
    SortUIChildren(go, children);
    for (std::size_t i = 0; i < children.size(); ++i)
        RenderCanvasRecursive(*children[i], childTransform, renderer, resources, ctx,
                              canvasToClip, pso, layer, true, depth + 1);
}

// ── サブシステム ──────────────────────────────────────────────────────────────
void UILayoutSystem(const std::vector<CanvasEntry>& canvases, UISystemContext& ctx,
                    float viewportWidth, float viewportHeight)
{
    for (const CanvasEntry& entry : canvases) {
        const float scale = ResolveCanvasScale(*entry.canvas, viewportWidth, viewportHeight);
        ApplyUILayoutRecursive(*entry.go, scale, ctx);
    }
}

void UIEventSystem(const std::vector<CanvasEntry>& canvases,
                   UISystemContext& ctx,
                   float viewportWidth,
                   float viewportHeight,
                   math::Vector2 rawMouseInViewport,
                   bool mousePressed,
                   const math::Matrix4& viewProjection,
                   math::Vector3 cameraWorldPos,
                   UIRenderTargetView targetView)
{
    // Canvas 間でも描画順の逆から入力を解決し、最前面で消費された入力を背面へ渡さない。
    bool inputConsumed = false;
    for (auto canvasIt = canvases.rbegin(); canvasIt != canvases.rend(); ++canvasIt) {
        const CanvasEntry& entry = *canvasIt;
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;

        // ── WorldSpace ヒット判定: スクリーン座標→ワールドレイ→キャンバスピクセル ──
        if (!IsScreenSpaceRenderMode(entry.canvas->renderMode)) {
            // 1. NDC マウス座標を計算してワールドレイを構築する
            const float ndcX =  (rawMouseInViewport.x / (std::max)(1.0f, viewportWidth))  * 2.0f - 1.0f;
            const float ndcY = -(rawMouseInViewport.y / (std::max)(1.0f, viewportHeight)) * 2.0f + 1.0f;
            const math::Matrix4 invVP = math::Matrix4::Inverse(viewProjection);
            const math::Ray worldRay = math::Ray::FromNDC(ndcX, ndcY, cameraWorldPos, invVP);

            // 2. キャンバスのワールド行列と法線平面を構築する
            const math::Matrix4 worldMat = math::Matrix4::TRS(
                entry.go->transform.worldPosition,
                entry.go->transform.worldRotation,
                math::Vector3::ONE);
            // Z 列 = キャンバス平面の法線 (ローカル前方がワールド空間でどの方向か)
            const math::Vector3 normal = {
                worldMat.m[0][2], worldMat.m[1][2], worldMat.m[2][2]
            };
            const math::Plane plane = math::Plane::FromNormalAndPoint(
                normal, entry.go->transform.worldPosition);

            // 3. レイと平面の交差判定
            float t;
            if (!worldRay.IntersectPlane(plane, t) || t < 0.0f) continue;
            const math::Vector3 hitWorld = worldRay.At(t);

            // 4. ヒット点をキャンバスピクセル座標へ逆変換する
            // worldMat^-1 を手計算: 純粋な TRS (scale=1) なら  localPos = invWorldMat * hitWorld
            const math::Matrix4 invWorld = math::Matrix4::Inverse(worldMat);
            const math::Vector4 hitLocal = MulMV(invWorld,
                { hitWorld.x, hitWorld.y, hitWorld.z, 1.0f });

            const float ws = entry.canvas->worldScale;
            const math::Vector2 canvasPx = {
                 hitLocal.x / ws + entry.canvas->canvasWidth  * 0.5f,
                -hitLocal.y / ws + entry.canvas->canvasHeight * 0.5f
            };

            // Canvas 直下の子から見た「親」は Canvas そのもの。
            // ここを 0 のままにするとアンカーが常に左上へ潰れる。
            UITransform2D canvasRoot{};
            canvasRoot.parentSize = { entry.canvas->canvasWidth, entry.canvas->canvasHeight };
            inputConsumed |= ProcessUIEventsRecursive(*entry.go, ctx, canvasRoot, canvasPx,
                                                      mousePressed, !inputConsumed, false);
            continue;
        }

        // ── ScreenSpace (Overlay / ScreenSpaceCamera) ──────────────────────────
        // NOTE: BuildCanvasRuntimeState を呼ばず直接計算する (cameraWorldRot 不要)
        float visibleW = 1.0f, visibleH = 1.0f;
        ResolveScreenSpaceCanvasArea(*entry.canvas, viewportWidth, viewportHeight, visibleW, visibleH);
        const float scaleX = (std::max)(1.0f, viewportWidth) / visibleW;
        const float scaleY = (std::max)(1.0f, viewportHeight) / visibleH;
        const math::Vector2 mouseInCanvas = {
            rawMouseInViewport.x / scaleX, rawMouseInViewport.y / scaleY
        };

        UITransform2D canvasRoot{};
        canvasRoot.parentSize = { entry.canvas->canvasWidth, entry.canvas->canvasHeight };
        inputConsumed |= ProcessUIEventsRecursive(*entry.go, ctx, canvasRoot, mouseInCanvas,
                                                  mousePressed, !inputConsumed, false);
    }
}

void UIRenderSystem(const std::vector<CanvasEntry>& canvases,
                    renderer::IRenderer& renderer,
                    renderer::ResourceManager& resources,
                    UISystemContext& ctx,
                    float viewportWidth,
                    float viewportHeight,
                    math::Vector2 rawMouseInViewport,
                    math::Vector3 cameraWorldPos,
                    math::Quaternion cameraWorldRot,
                    const math::Matrix4& viewProjection,
                    UIRenderTargetView targetView)
{
    renderer.SetSampler(5, renderer::SamplerMode::CLAMP_LINEAR);

    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go,
            viewportWidth, viewportHeight,
            rawMouseInViewport, viewProjection,
            cameraWorldPos, cameraWorldRot, ctx, targetView);
        UITransform2D canvasRoot{};
        canvasRoot.parentSize = { entry.canvas->canvasWidth, entry.canvas->canvasHeight };
        RenderCanvasRecursive(*entry.go, canvasRoot, renderer, resources, ctx,
                              state.canvasToClip, state.pso, state.layer, false);
    }
}

} // namespace (anonymous)

// ── 公開 API ──────────────────────────────────────────────────────────────────
void UISystemSetDefaultFontPath(UISystemContext& ctx, const std::string& basePath)
{
    ctx.defaultFontPath = basePath;
}

void UISystemFlushCache(UISystemContext& ctx)
{
    // WHY: シーン破棄時・アセットリロード時に呼び出して、古い FontAtlas テクスチャハンドルが
    //      破棄済み ResourceManager を参照し続けるのを防ぐ。
    ctx.fontAtlasCache.clear();
    // マテリアルも同じ理由で捨てる。加えて、.mat やシェーダーを編集したときに
    // ここを通ることで反映される (解決結果は失敗も含めてキャッシュしているため)。
    ctx.materialCache.clear();
    // 子リストの作業領域は破棄済み GameObject を指したままになりうる。
    // 中身だけ捨てて、確保済みの容量は次のシーンでそのまま使い回す。
    for (auto& children : ctx.childScratch) children.clear();
}

void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              UISystemContext& ctx,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInViewport,
              bool mousePressed,
              math::Vector3    cameraWorldPos,
              math::Quaternion cameraWorldRot,
              const math::Matrix4& viewProjection,
              UIRenderTargetView targetView)
{
    EnsureInit(ctx, resources);

    // 頂点バッファの貸し出しを巻き戻す。フレームが変わったときだけ行うのは、
    // 同じフレーム内で 1 つの Context が複数回描いても実体が衝突しないようにするため。
    if (ctx.lastResetFrame != fbzz::Time::frameCount) {
        ctx.lastResetFrame    = fbzz::Time::frameCount;
        ctx.imageVertexCursor = 0;
        ctx.textVertexCursor  = 0;
    }

    if (!ctx.shader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() || !ctx.whiteTexture.IsValid())
        return;

    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    UITextSizeSystem(canvases, ctx, resources);
    UILayoutSystem(canvases, ctx, viewportWidth, viewportHeight);
    // WHY: Editorは同じSceneをScene / Game / Canvas Editorへ1フレーム中に複数回描画する。
    //      各ViewportでUIButtonのlastMouseStateやonClickを更新すると、後続の描画パスが
    //      Game Viewportで生成したクリックイベントを消してしまう。ゲーム入力を受ける
    //      GameViewportだけが共有Componentのイベント状態を更新する。
    if (targetView == UIRenderTargetView::GameViewport) {
        UIEventSystem(canvases, ctx, viewportWidth, viewportHeight, mouseInViewport,
                      mousePressed, viewProjection, cameraWorldPos, targetView);
    }
    UIRenderSystem(canvases, renderer, resources, ctx,
                   viewportWidth, viewportHeight, mouseInViewport,
                   cameraWorldPos, cameraWorldRot, viewProjection, targetView);
}

} // namespace fbzz::scene
