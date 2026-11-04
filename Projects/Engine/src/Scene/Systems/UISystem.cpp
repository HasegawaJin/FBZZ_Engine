/// @file    UISystem.cpp
/// @brief   CPU で確定した UI 矩形の GPU 描画と互換呼び出しの接続。
/// @author  Hasegawa Jin
/// @date    2026-05-23

/// @note UICanvas / UIImage / UIText / UIButton を走査し、DrawCall と hit 状態を作る。
/// @note ScreenSpace / ScreenSpaceCamera / WorldSpace の 3 モードを扱う。
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/UIThemeAsset.hpp>
#include <Engine/Scene/UITheme.hpp>
#include <Engine/Scene/Components/UIRect.hpp>
#include "Engine/Scene/Systems/UISystem.hpp"
#include "UIRuntimeGeometry.hpp"
#include "UITextLayout.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/UICanvas.hpp"
#include "Engine/Scene/Components/UICanvasGroup.hpp"
#include "Engine/Scene/Components/UIImage.hpp"
#include "Engine/Scene/Components/UIButton.hpp"
#include "Engine/Scene/Components/UIText.hpp"
#include "Engine/Scene/Components/UILayoutGroup.hpp"
#include "Engine/Scene/Components/UIControls.hpp"
#include "Engine/Scene/Components/UIElement.hpp"
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
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Util/Utf8.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {
using namespace ui_runtime_detail;

/// @note 頂点レイアウトの定義は UISystem.hpp (UIVertex2D)。作業領域を Context へ
/// @note 置くためにヘッダー側へ出してあるので、ここでは名前だけ借りる。
using UIVertex = UIVertex2D;

/// @note 定義は UISystem.hpp (UIConstantsCB)。エディタの UI マテリアルプレビューと共有する。
using UIConstants = UIConstantsCB;


struct CanvasRuntimeState {
    math::Matrix4 canvasToClip = math::Matrix4::Identity();
    math::Vector2 mouseInCanvasSpace = {};
    renderer::ResourceHandle<renderer::PipelineStateTag> pso;
    renderer::RenderLayer layer = renderer::RenderLayer::OVERLAY_LAYER;
};

/// @note 頂点バッファの固定容量。
/// @note 矩形は 6 頂点で足りるが、Radial の扇形切りで 24 まで太り、Mask / Scroll View の
/// @note クリップは半平面 1 枚につき頂点を 1 つ増やす。借用制のバッファは後から広げられない
/// @note ので最大側で確保する。64 は 3 段の入れ子 (12 枚) までは切り落とされない数。
static constexpr uint32_t kImageVBVertices = 64;
/// @note 1 つの多角形を切るときの作業配列の長さ。上と同じ理由で余裕を持たせる。
static constexpr int kMaxClipPolygonVertices = 32;
static constexpr uint32_t kTextVBVertices  = 4096; ///< @note ~682 グリフ分。超過時は複数ドローに分割する


/// @name 多角形の切り取り
/// @note 扇形の塗り潰しも Mask / Scroll View のクリップも「凸多角形を半平面で削る」でしかない。
struct ClipVertex {
    math::Vector2 local;   ///< @note Canvas 空間、または要素ローカル (使う側で揃える)
    math::Vector2 uv;      ///< @note セル内 0..1 またはアトラス UV
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

/// @note dot(p - point, normal) >= 0 側を残す (Sutherland-Hodgman)。
/// @note 凸多角形の入力に対し、出力は入力 + 1 頂点までしか増えない。
int ClipHalfPlane(const ClipVertex* in, int count, const math::Vector2& point,
                  const math::Vector2& normal, ClipVertex* out, int outCapacity)
{
    int outCount = 0;
    const auto emit = [&](const ClipVertex& v) {
        if (outCount < outCapacity) out[outCount++] = v;
    };
    for (int i = 0; i < count; ++i) {
        const ClipVertex& a = in[i];
        const ClipVertex& b = in[(i + 1) % count];
        const float da = (a.local.x - point.x) * normal.x + (a.local.y - point.y) * normal.y;
        const float db = (b.local.x - point.x) * normal.x + (b.local.y - point.y) * normal.y;
        if (da >= 0.0f) emit(a);
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            emit({
                { a.local.x + (b.local.x - a.local.x) * t,
                  a.local.y + (b.local.y - a.local.y) * t },
                { a.uv.x + (b.uv.x - a.uv.x) * t,
                  a.uv.y + (b.uv.y - a.uv.y) * t },
                { a.color.x + (b.color.x - a.color.x) * t,
                  a.color.y + (b.color.y - a.color.y) * t,
                  a.color.z + (b.color.z - a.color.z) * t,
                  a.color.w + (b.color.w - a.color.w) * t }
            });
        }
    }
    return outCount;
}

/// @note いま積まれているクリップ面すべてで多角形を削る。頂点は Canvas 空間で渡すこと。
/// @note 何も残らなければ 0。1 枚ずつ順に当てるのは、共通部分の定義がそのまま
/// @note 「全半平面を満たす点」だから。矩形どうしの交差として畳むと回転で表せなくなる。
int ClipPolygonToPlanes(const std::vector<UIClipPlane>& planes,
                        const ClipVertex* in, int count,
                        ClipVertex* out, int outCapacity)
{
    if (planes.empty()) {
        const int copied = (std::min)(count, outCapacity);
        for (int i = 0; i < copied; ++i) out[i] = in[i];
        return copied;
    }
    ClipVertex bufferA[kMaxClipPolygonVertices];
    ClipVertex bufferB[kMaxClipPolygonVertices];
    int n = (std::min)(count, kMaxClipPolygonVertices);
    for (int i = 0; i < n; ++i) bufferA[i] = in[i];

    ClipVertex* src = bufferA;
    ClipVertex* dst = bufferB;
    for (const UIClipPlane& plane : planes) {
        n = ClipHalfPlane(src, n, plane.point, plane.normal, dst, kMaxClipPolygonVertices);
        if (n < 3) return 0;
        std::swap(src, dst);
    }
    const int copied = (std::min)(n, outCapacity);
    for (int i = 0; i < copied; ++i) out[i] = src[i];
    return copied;
}

/// @note 矩形 (回転あり) を内向き 4 面としてクリップスタックへ積む。
/// @note 戻り値は積んだ枚数。抜けるときに同じ数だけ外す。
std::size_t PushClipRect(std::vector<UIClipPlane>& planes, const Rect& rect, float rotationZ)
{
    const float c = std::cosf(rotationZ);
    const float s = std::sinf(rotationZ);
    /// @note 回転の中心は要素の中心。描画側 (LocalToCanvas) と同じ規則で揃える。
    const math::Vector2 center = { rect.pos.x + rect.size.x * 0.5f,
                                   rect.pos.y + rect.size.y * 0.5f };
    /// @note 辺の外向き法線を回した 4 本。内側に残したいので符号を反転して積む。
    /// @note 要素ローカルの +x が Canvas 上で向く方向
    const math::Vector2 axisX = { c, s };
    /// @note 同 +y
    const math::Vector2 axisY = { -s, c };
    const float halfW = rect.size.x * 0.5f;
    const float halfH = rect.size.y * 0.5f;

    const auto push = [&](const math::Vector2& normal, float halfExtent) {
        /// @note 面上の 1 点 = 中心から法線と逆向きへ halfExtent 進んだところ。
        planes.push_back({ { center.x - normal.x * halfExtent,
                             center.y - normal.y * halfExtent }, normal });
    };
    /// @note 左辺 (内向き = +x)
    push(axisX, halfW);
    /// @note 右辺
    push({ -axisX.x, -axisX.y }, halfW);
    /// @note 上辺 (内向き = +y)
    push(axisY, halfH);
    /// @note 下辺
    push({ -axisY.x, -axisY.y }, halfH);
    return 4;
}

/// @name UI マテリアル解決
/// @note メッシュ側の SyncMaterial は MaterialComponent を入口に持つので、同じ入口に載せると
/// @note UIImage 全部へ MaterialComponent を要求することになる (UI に submesh は無く、
/// @note .mat を持たない要素が多数)。共有するのは入口ではなく「.mat をシェーダーへ束縛する
/// @note 規則」の方で、そちらは Engine/Asset/MaterialParamBinding.hpp にある。
UIMaterialBinding* ResolveUIMaterial(UISystemContext& ctx,
                                     renderer::ResourceManager& resources,
                                     const std::string& materialPath)
{
    if (materialPath.empty()) return nullptr;

    if (const auto it = ctx.materialCache.find(materialPath); it != ctx.materialCache.end()) {
        auto& cached = it->second;
        if (!cached.valid) return nullptr;
        for (size_t i = 0; i < cached.texturePaths.size(); ++i)
            cached.material.textures[i] = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, cached.texturePaths[i]);
        /// @note 非同期完了・品質変更で texture mask と bindless 添字も変わりうる。
        cached.material.Upload(resources, cached.descriptor);
        return &cached;
    }

    /// @note 失敗も含めてキャッシュへ入れる。読めない .mat を毎フレーム開き直すと、
    /// @note 壊れた 1 件がフレーム時間を持っていくうえログが埋まる。
    UIMaterialBinding& binding = ctx.materialCache[materialPath];

    const auto assetHandle = asset::AssetManager::Load<asset::MaterialAsset>(materialPath);
    const auto matAssetAuthoringSnapshot = assetHandle.IsValid()
        ? asset::ReadMaterialAuthoring(assetHandle) : asset::MaterialAsset::AuthoringSnapshotPtr{};
    const auto* matAsset = matAssetAuthoringSnapshot ? &matAssetAuthoringSnapshot->Data() : nullptr;
    if (!matAsset) {
        FBZZ_LOG_WARN("UI material load failed '%s' -> falling back to the built-in sprite shader.",
                      materialPath.c_str());
        return nullptr;
    }

    /// @note メッシュ用の .mat は b0 を CameraConstants として読むが、UI パスはそこへ ortho を
    /// @note 入れる。割り当てると頂点が飛ぶか真っ黒になり、絵からは原因が分からない。
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

    /// @note 記述子はここで値ごと持つ。シェーダーは ResourceManager がホットリロードで
    /// @note 差し替えうるので、ポインタで持つと解決時の中身と食い違う瞬間ができる。
    if (auto* shader = resources.Get(material.shader))
        binding.descriptor = shader->GetDescriptor();

    material.paramData.assign(binding.descriptor.cbufferSize, 0u);
    if (binding.descriptor.IsValid()) {
        asset::InitDefaultMaterialParams(binding.descriptor, material.paramData);
        asset::ApplyMaterialAssetParams(*matAsset, binding.descriptor, material.paramData);
    }

    const auto texturePaths = asset::ResolveMaterialTexturePaths(*matAsset);
    binding.texturePaths.assign(texturePaths.begin(), texturePaths.end());
    material.textures.resize(texturePaths.size());
    for (size_t i = 0; i < texturePaths.size(); ++i) {
        material.textures[i] = texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePaths[i]);
    }

    material.Init(resources, binding.descriptor.cbufferSize);
    material.Upload(resources, binding.descriptor);

    if (binding.descriptor.cbufferSize > 0) {
        /// @note 上書きを持つ要素だけが使う。持たない要素は共有の paramsBuffer を直接読む。
        /// @note 作業領域は解決時に確保しきる。描画中に伸ばすことは無い。
        binding.overrideScratch.resize(binding.descriptor.cbufferSize);
        binding.overrideConstants = resources.CreateConstantBuffer(binding.descriptor.cbufferSize);
    }

    /// @note UI は常に深度を書かない。ScreenSpace は深度テストもしない、
    /// @note WorldSpace は 3D の手前後関係に従う ─ 既定 PSO と同じ使い分けを blend だけ差し替える。
    binding.screenPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, matAsset->blendMode, renderer::DepthMode::DEPTH_OFF });
    binding.worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, matAsset->blendMode, renderer::DepthMode::DEPTH_READ });
    binding.valid    = true;
    return &binding;
}

/// @note 要素ごとの上書きを DrawCall へ適用する。上書きが無ければ何もしない。
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
            if (const auto texture = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, it->second); texture.IsValid())
                call.textures[slot] = texture;
        }
    }

    if (image.materialParamOverrides.empty()) return;
    if (!binding.overrideConstants.IsValid()) return;
    if (binding.overrideScratch.size() != binding.material.paramData.size()) return;

    /// @note 共有マテリアルの値を土台に、この要素ぶんだけ重ねて別の cbuffer へ流す。
    /// @note 共有側の paramData は触らない (同じ .mat を使う他の要素へ波及する)。
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

template<typename Tag>
void ReleaseUIHandle(renderer::ResourceHandle<Tag>& handle)
{
    if (auto* owner = renderer::ResourceManager::FindOwner(handle.owner)) owner->Release(handle);
    handle = {};
}

void ReleaseMaterialCache(UISystemContext& ctx)
{
    for (auto& [key, binding] : ctx.materialCache) {
        ReleaseUIHandle(binding.overrideConstants);
        ReleaseUIHandle(binding.screenPso);
        ReleaseUIHandle(binding.worldPso);
    }
    ctx.materialCache.clear();
}

/// @brief キャッシュの作り直しが要る «外の変化» を拾う。
/// @note UISystemFlushCache を呼ぶべき瞬間 (デバイスロスト・アセットの再取り込み) を知って
/// @note いるのは呼び出し側 (Editor / Runtime) ではなく、版数を持つ ResourceManager と
/// @note AssetManager。呼び出し側に «忘れずに呼ぶ» を期待すると «UI の .mat を編集しても
/// @note 再起動まで反映されない» が長く残ったため、入口で版数を突き合わせて自分で捨てる。
void SyncCacheGenerations(UISystemContext& ctx, renderer::ResourceManager& resources)
{
    const std::uint64_t resetVersion    = resources.GetResetVersion();
    const std::uint64_t shaderVersion   = resources.GetShaderVersion();
    const int           assetGeneration = asset::AssetManager::GetAssetGeneration();

    if (ctx.cachedResourceOwner != resources.GetInstanceId() || ctx.cachedResetVersion != resetVersion) {
        UISystemReleaseGpuResources(ctx);
        ctx.cachedResourceOwner   = resources.GetInstanceId();
        ctx.cachedResetVersion    = resetVersion;
        ctx.cachedShaderVersion   = shaderVersion;
        ctx.cachedAssetGeneration = assetGeneration;
        /// @note EnsureInit が作ったシェーダー・PSO・白テクスチャも同じ世代のもの。
        /// @note 作り直させないと、以降ずっと死んだハンドルで描き続ける。
        ctx.initialized = false;
        return;
    }

    if (ctx.cachedAssetGeneration != assetGeneration || ctx.cachedShaderVersion != shaderVersion) {
        ctx.cachedAssetGeneration = assetGeneration;
        ctx.cachedShaderVersion = shaderVersion;
        /// @note マテリアルだけを捨てる。フォントアトラスは .ttf / .fnt を直接読んでおり
        /// @note AssetManager のストアに載っていない。どのアセットが変わっても版数は動くので、
        /// @note アトラスまで捨てると «無関係な .png を保存しただけ» で動的 SDF の焼き直しが
        /// @note 走る (体感できるほど止まる)。
        ReleaseMaterialCache(ctx);
    }
}

/// @name UISystemContext 初期化
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
    /// @note WorldSpace / ScreenSpaceCamera UI: 深度テストあり・深度書き込みなし
    ctx.worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    /// @note 選択マスク: 塗るのは被覆だけなので不透明。深度は書き込む (理由は UISystem.hpp を参照)。
    ctx.selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });

    /// @note 共有の 1x1 白を借りる。ここで作ると、コンテキストの数だけ・デバイスリセットの
    /// @note 数だけ «誰も返さない 1 枚» が増える。
    ctx.whiteTexture = resources.GetWhiteTexture();

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

void BeginUIFrame(UISystemContext& ctx)
{
    /// @note クリップは階層を降りるあいだだけ積むので、抜けれ切れば空になる。
    /// @note それでも入口で均すのは、途中で return した経路が面を残さないため。
    ctx.clipPlanes.clear();
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

    /// @name WorldSpace
    if (canvas.renderMode == UIRenderMode::WorldSpace) {
        const float ws = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  ws;
        pixelToLocal.m[1][1] = -ws;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * ws;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * ws;

        /// @note Canvas が子 GO に置かれることがあるので worldPosition を使う。
        /// @note faceCamera は向きだけカメラ姿勢で上書きする (位置は追従したまま正対する)。
        const math::Quaternion canvasRotation =
            canvas.faceCamera ? cameraWorldRot : canvasGO.transform.worldRotation;
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasGO.transform.worldPosition - canvasRotation * math::Vector3::FORWARD * canvas.surfaceOffset,
            canvasRotation,
            math::Vector3::ONE
        );

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        /// @note WorldSpace はレイキャストで処理
        state.mouseInCanvasSpace = rawMouseInViewport;
        state.pso   = canvas.depthMode == UICanvasDepthMode::ALWAYS_ON_TOP ? ctx.pso : ctx.worldPso;
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    /// @name ScreenSpaceCamera
    if (canvas.renderMode == UIRenderMode::ScreenSpaceCamera) {
        /// @note Canvas をカメラ前方 planeDistance に置いてカメラ向きで固定する。
        /// @note Overlay と同じスクリーン UI に深度テストを足す用途。マウス座標系は Overlay と同じ。
        const float ws = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  ws;
        pixelToLocal.m[1][1] = -ws;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * ws;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * ws;

        /// @note カメラ前方ベクトル: worldRot * (0, 0, 1)
        const math::Vector3 camFwd = cameraWorldRot * math::Vector3{ 0.0f, 0.0f, 1.0f };
        const math::Vector3 canvasPos = cameraWorldPos + camFwd * (canvas.planeDistance - canvas.surfaceOffset);
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasPos, cameraWorldRot, math::Vector3::ONE);

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        /// @note マウス座標: Overlay と同じスクリーン→キャンバス変換
        float visibleW = 1.0f, visibleH = 1.0f;
        ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, visibleW, visibleH);
        const float scaleX = (std::max)(1.0f, viewportWidth) / visibleW;
        const float scaleY = (std::max)(1.0f, viewportHeight) / visibleH;
        state.mouseInCanvasSpace = { rawMouseInViewport.x / scaleX, rawMouseInViewport.y / scaleY };
        /// @note 深度テストあり (3D オブジェクトに遮蔽可)
        state.pso   = canvas.depthMode == UICanvasDepthMode::ALWAYS_ON_TOP ? ctx.pso : ctx.worldPso;
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    /// @name ScreenSpaceOverlay (デフォルト)
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



/// @name 描画サブミット
/// @note 組み上がった三角形リストを 1 ドローとして積む。頂点を呼び出し側から受け取るのは、
/// @note 矩形しか描けない形にすると Radial の凸多角形が別実装になり、マテリアル適用と
/// @note 定数バッファの組み立てが二重になるため。
/// @note vertices の uv は「このセルの中の 0..1」。アトラス上の位置は uvMin/uvMax が持つ。
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

    /// @note この DrawCall 専用の頂点バッファを借りる (UISystemContext::vertexPool 参照)。
    const auto vertexBuffer = ctx.vertexPool.Acquire(
        resources, kImageVBVertices, static_cast<uint32_t>(sizeof(UIVertex)));
    if (!vertexBuffer.IsValid()) return;
    resources.Update(vertexBuffer, vertices, vertexCount * sizeof(UIVertex));

    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = color;
    constants.uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
    /// @note 9-slice やタイルはセルごとにここへ来るので、セルの寸法がそのまま渡る。
    /// @note 角丸マテリアルを Sliced / Tiled と併用してはいけないのはこのため。
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
        /// @note .mat の blend_mode を焼いた PSO。作れていなければ既定の PSO のままにする。
        const auto materialPso = worldSpace ? material->worldPso : material->screenPso;
        if (materialPso.IsValid()) call.pipelineState = materialPso;
        /// @note b2 = MaterialConstants。UICommon.hlsli が宣言するレジスタと 1 対 1。
        call.constantBuffers[2] = resolved.paramsBuffer;
        /// @note 図形をシェーダーで描く UI マテリアルは albedo を持たないことが多い。
        /// @note 無条件に上書きすると UIImage 側で指定した絵が黙って消える。
        for (size_t slot = 0; slot < resolved.textures.size() && slot < call.textures.size(); ++slot) {
            if (resolved.textures[slot].IsValid())
                call.textures[slot] = resolved.textures[slot];
        }
        /// @note 共有 .mat を適用したうえに、この要素だけの差分を重ねる。
        if (overrideSource)
            ApplyUIMaterialOverrides(resources, *material, *overrideSource, call);
    }
    renderer.Submit(call, resources);
}

/// @note 凸多角形を 1 枚。いま積まれているクリップ面で削ってから三角形へ開く。
/// @note 全経路をここへ通さないと「マスクの中で 9-slice だけはみ出す」形の穴になる。
/// @note 矩形・扇形・グリフの違いは多角形を作るところまでで、そこから先は同じ。
void SubmitClippedPolygon(renderer::IRenderer& renderer,
                          renderer::ResourceManager& resources,
                          UISystemContext& ctx,
                          const math::Matrix4& canvasToClip,
                          renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                          renderer::RenderLayer layer,
                          const ClipVertex* polygon,
                          int polygonCount,
                          const math::Vector2& cellSize,
                          const math::Vector4& color,
                          const math::Vector2& uvMin,
                          const math::Vector2& uvMax,
                          renderer::ResourceHandle<renderer::TextureTag> texture,
                          UIMaterialBinding* material,
                          bool worldSpace,
                          const UIImage* overrideSource)
{
    if (polygonCount < 3) return;

    ClipVertex clipped[kMaxClipPolygonVertices];
    const int count = ClipPolygonToPlanes(ctx.clipPlanes, polygon, polygonCount,
                                          clipped, kMaxClipPolygonVertices);
    if (count < 3) return;

    /// @note 扇状に開く。凸多角形なので、どの頂点を軸にしても裏返らない。
    UIVertex vertices[kImageVBVertices];
    uint32_t vertexCount = 0;
    for (int t = 1; t + 1 < count; ++t) {
        if (vertexCount + 3 > kImageVBVertices) break;
        vertices[vertexCount++] = { clipped[0].local,     clipped[0].uv,     clipped[0].color };
        vertices[vertexCount++] = { clipped[t].local,     clipped[t].uv,     clipped[t].color };
        vertices[vertexCount++] = { clipped[t + 1].local, clipped[t + 1].uv, clipped[t + 1].color };
    }
    SubmitUIGeometry(renderer, resources, ctx, canvasToClip, pso, layer,
                     vertices, vertexCount, cellSize, color, uvMin, uvMax,
                     texture, material, worldSpace, overrideSource);
}

/// @note 矩形 1 枚。回転は中心まわり。
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
                 /// @note 上書きの持ち主。マテリアルを使わない描画では nullptr。
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

    /// @note 頂点 UV は 0..1。アトラス UV を直接積むと頂点シェーダーの g_UVRect と二重写像になり、
    /// @note 切り出し矩形を指定したとき内側の一部しかサンプリングされない。
    /// @note この 0..1 は角丸などの図形を描くための矩形内座標も兼ねる。
    /// @note 巻き順は左上 → 左下 → 右下 → 右上。切り取りが凸多角形前提なので 4 頂点で渡す。
    const ClipVertex polygon[4] = {
        { rot(-hW, -hH), { 0.0f, 0.0f } },
        { rot(-hW,  hH), { 0.0f, 1.0f } },
        { rot( hW,  hH), { 1.0f, 1.0f } },
        { rot( hW, -hH), { 1.0f, 0.0f } },
    };
    SubmitClippedPolygon(renderer, resources, ctx, canvasToClip, pso, layer,
                         polygon, 4, size, color, uvMin, uvMax,
                         texture, material, worldSpace, overrideSource);
}

/// @name 画像セル
/// @note 9-slice もタイルも「元画像の一部を要素の一部へ貼る」の繰り返し。セルへ揃えることで
/// @note 塗り潰しのクリップを 1 か所に書けば Simple / Sliced / Tiled 全部に効く。
/// @note pos は要素ローカル (左上原点・回転前)。回転は描画直前にまとめて掛ける。
struct ImageCell {
    math::Vector2 pos;
    math::Vector2 size;
    math::Vector2 uvMin;
    math::Vector2 uvMax;
};

/// @note 1 要素を描くのに要る、セル間で変わらない値。セル単位の関数へ 14 個の引数を並べると
/// @note 順番違いが型で止まらないので束ねる。
/// @note メンバー名 renderer が名前空間 renderer と重なるため、型は fbzz:: から書く。
struct ImageDrawContext {
    fbzz::renderer::IRenderer&       renderer;
    fbzz::renderer::ResourceManager& resources;
    UISystemContext&                 ctx;
    const math::Matrix4&             canvasToClip;
    fbzz::renderer::ResourceHandle<fbzz::renderer::PipelineStateTag> pso;
    fbzz::renderer::RenderLayer      layer;
    math::Vector2                    origin;      ///< @note 要素の左上 (Canvas 空間)
    math::Vector2                    elementSize;
    float                            cosZ;
    float                            sinZ;
    math::Vector4                    color;
    fbzz::renderer::ResourceHandle<fbzz::renderer::TextureTag> texture;
    UIMaterialBinding*               material;
    bool                             worldSpace;
    const UIImage*                   overrideSource;
};

/// @note 要素ローカル座標を Canvas 座標へ。回転の中心は常に要素全体の中心。
/// @note セルごとに回すと 9-slice の四隅がばらばらに回って継ぎ目が開く。
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
    const ClipVertex polygon[4] = {
        { LocalToCanvas(draw, cell.pos), { 0.0f, 0.0f } },
        { LocalToCanvas(draw, { cell.pos.x, cell.pos.y + cell.size.y }), { 0.0f, 1.0f } },
        { LocalToCanvas(draw, { cell.pos.x + cell.size.x, cell.pos.y + cell.size.y }),
          { 1.0f, 1.0f } },
        { LocalToCanvas(draw, { cell.pos.x + cell.size.x, cell.pos.y }), { 1.0f, 0.0f } },
    };
    SubmitClippedPolygon(draw.renderer, draw.resources, draw.ctx, draw.canvasToClip,
                         draw.pso, draw.layer, polygon, 4, cell.size, draw.color,
                         cell.uvMin, cell.uvMax, draw.texture, draw.material,
                         draw.worldSpace, draw.overrideSource);
}

/// @note 9-slice の 1 軸ぶんの境界 (4 本) と、対応する UV を作る。
/// @note multiplier が描画側だけを拡縮するので、元画像側と描画側は比例しない。
void BuildSliceAxis(float elementLength, float uvLow, float uvHigh, float texturePixels,
                    float borderLow, float borderHigh, float unitScale,
                    float* outEdge, float* outUv)
{
    const float sourceLength = (uvHigh - uvLow) * texturePixels;
    /// @note 余白どうしが重なると中央が裏返る。まず元画像側で収める。
    const float sourceLow  = std::clamp(borderLow, 0.0f, sourceLength * 0.5f);
    const float sourceHigh = std::clamp(borderHigh, 0.0f, sourceLength - sourceLow);
    /// @note 描画側は要素をはみ出せない。狭い要素では余白どうしが先に突き当たる。
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

/// @note 1 つの領域を step 間隔で敷き詰め、タイル 1 枚ずつを emit へ渡す。
/// @note step が 0 以下の軸は分割しない (辺の帯を一方向だけタイルするのに使う)。
/// @note 数ピクセルの素材を全画面へ敷くと 1 要素で数万ドローになるので上限を置き、
/// @note 当たったらタイルを引き伸ばす。密度はずれるが隙間が空くよりは意図が伝わる。
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
        /// @note 縦横の比を保ったまま総数を落とす。片方だけ削ると模様の目が伸びる。
        const float shrink = std::sqrt(
            static_cast<float>(countX * countY) / static_cast<float>(kMaxTilesPerRegion));
        countX = (std::max)(1, static_cast<int>(static_cast<float>(countX) / shrink));
        countY = (std::max)(1, static_cast<int>(static_cast<float>(countY) / shrink));
    }
    /// @note 上限に当たった軸は「領域を count 等分した幅」まで広げる。当たっていなければ step のまま。
    const float spanX = (std::max)(stepX, region.size.x / static_cast<float>(countX));
    const float spanY = (std::max)(stepY, region.size.y / static_cast<float>(countY));

    const float du = region.uvMax.x - region.uvMin.x;
    const float dv = region.uvMax.y - region.uvMin.y;
    for (int row = 0; row < countY; ++row) {
        for (int column = 0; column < countX; ++column) {
            ImageCell tile;
            tile.pos = { region.pos.x + spanX * static_cast<float>(column),
                         region.pos.y + spanY * static_cast<float>(row) };
            /// @note 端の 1 枚は途中で切れる。切れた分だけ UV も詰める。
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

/// @note imageType に応じてセルを列挙する。
/// @note Simple … 1 枚
/// @note Sliced … 最大 9 枚 (四隅は原寸、辺と中央は引き伸ばし)
/// @note Tiled  … Border があれば四隅は原寸のまま辺と中央をタイル、無ければ全面をタイル
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
        /// @note 余白が全部 0 なら格子を作る意味が無い。Sliced は 1 枚、Tiled は全面タイルへ落ちる。
        if (border.x > 0.0f || border.y > 0.0f || border.z > 0.0f || border.w > 0.0f) {
            float x[4], u[4], y[4], v[4];
            BuildSliceAxis(size.x, uvMin.x, uvMax.x, textureSize.x,
                           border.x, border.z, unitScale, x, u);
            BuildSliceAxis(size.y, uvMin.y, uvMax.y, textureSize.y,
                           border.y, border.w, unitScale, y, v);
            /// @note 中央列 / 中央行の元画像での寸法。Tiled のタイル 1 枚の大きさになる。
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
                    /// @note 四隅は原寸のまま。伸びる方向だけをタイルへ置き換える。
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

/// @note 塗り潰し (Edge) でセルを削る。何も残らなければ false。

/// @note 要素全体を 1 枚のセルとして渡せば、分割前と同じ「矩形と UV を端から削る」動きになる。
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

/// @name Radial 塗り潰し
/// @note 180 度以下の扇形は「中心を通る 2 つの半平面の共通部分」なので、セルをその 2 枚で
/// @note 切れば残りがそのまま描く形になる。近似ではないのでどの角度でも輪郭が正確に出る。
/// @note 道具 (ClipVertex / ClipHalfPlane) はファイル先頭で定義済み。

/// @note fillOrigin が指す辺の方向を角度で返す。Canvas は y が下向きなので、
/// @note 角度が増える向き = 画面上の時計回りになる。
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

/// @note 扇形の定義。amount = 1 でちょうど要素全体を覆う位置と角度になる。
struct RadialSector {
    math::Vector2 apex{};        ///< @note 要素ローカルの回転軸
    float startAngle = 0.0f;     ///< @note fillAmount = 0 のときの縁
    float fullSweep  = 0.0f;     ///< @note amount = 1 での角度 (符号は回転方向)
};

/// @note 90 / 180 は「四半円 / 半円で要素をちょうど覆う」形。中心を軸にしたまま角度だけ
/// @note 絞ると要素の一部しか覆えないので、軸を角 (90) や辺の中点 (180) へ移す。
RadialSector BuildRadialSector(UIImageFillMethod method, UIImageFillOrigin origin,
                               bool clockwise, const math::Vector2& size)
{
    constexpr float kPi = 3.14159265358979323846f;
    const float direction = clockwise ? 1.0f : -1.0f;
    const UIImageFillOrigin resolved = NormalizeFillOrigin(method, origin);

    RadialSector sector;
    switch (method) {
    case UIImageFillMethod::Radial90: {
        /// @note 角を軸に、そこから伸びる 2 辺の間を四半周する。
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
        /// @note 覆う範囲は [baseAngle, baseAngle + 90 度]。どちら端から満ちるかだけが向きで変わる。
        sector.startAngle = clockwise ? corner.baseAngle : corner.baseAngle + kPi * 0.5f;
        sector.fullSweep  = direction * kPi * 0.5f;
        break;
    }
    case UIImageFillMethod::Radial180: {
        /// @note 辺の中点を軸に、その辺に沿って半周する。内側へ向かう法線を必ず通る。
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
    /// @note セルが軸を含まなくても、半平面の切り取りは同じ式で効く。
    const math::Vector2 apex = sector.apex;
    const float base = sector.startAngle;
    /// @note 180 度を超える扇形は 2 枚の半平面では表せない。等分して 2 区画に分ける。
    const int chunkCount = sweep > kPi ? 2 : 1;
    const float chunk = sweep / static_cast<float>(chunkCount);

    /// @note 隣り合う区画は 1 本の境界を共有する。区画ごとに角度を足し引きすると
    /// @note 丸め誤差で 2 本に割れ、境目に隙間か二重塗りの筋が出る。
    float bounds[3] = {};
    for (int i = 0; i <= chunkCount; ++i) {
        const float offset = chunk * static_cast<float>(i);
        bounds[i] = clockwise ? base + offset : base - offset;
    }

    /// @note 区画ごとに submit する。2 区画をまとめて 1 つの多角形にはできない
    /// @note (合わせると凹むことがある)。区画は最大 2 つなので、ドローは高々 2 本。
    for (int i = 0; i < chunkCount; ++i) {
        /// @note 切り取りは常に「角度の小さい側 → 大きい側」で渡す。
        const float low  = clockwise ? bounds[i]     : bounds[i + 1];
        const float high = clockwise ? bounds[i + 1] : bounds[i];
        const math::Vector2 dirLow  = { std::cosf(low),  std::sinf(low)  };
        const math::Vector2 dirHigh = { std::cosf(high), std::sinf(high) };

        /// @note 内側の条件は cross(dirLow, v) >= 0 かつ cross(dirHigh, v) <= 0。
        /// @note cross(d, v) は dot(v, (-d.y, d.x)) と同じなので、法線 2 本に直せる。
        ClipVertex work[kMaxClipPolygonVertices];
        ClipVertex sectorPoly[kMaxClipPolygonVertices];
        int n = ClipHalfPlane(polygon, 4, apex, { -dirLow.y, dirLow.x },
                              work, kMaxClipPolygonVertices);
        n = ClipHalfPlane(work, n, apex, { dirHigh.y, -dirHigh.x },
                          sectorPoly, kMaxClipPolygonVertices);
        if (n < 3) continue;

        /// @note 扇形は要素ローカルで解いてある。Mask のクリップは Canvas 空間なので、
        /// @note 渡す前にここで写す (2 つの座標系を 1 つの関数に混ぜない)。
        for (int v = 0; v < n; ++v)
            sectorPoly[v].local = LocalToCanvas(draw, sectorPoly[v].local);

        SubmitClippedPolygon(draw.renderer, draw.resources, draw.ctx, draw.canvasToClip,
                             draw.pso, draw.layer, sectorPoly, n, cell.size, draw.color,
                             cell.uvMin, cell.uvMax, draw.texture, draw.material,
                             draw.worldSpace, draw.overrideSource);
    }
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

/// @name フォントアトラス
renderer::FontAtlas& GetOrLoadFontAtlas(const std::string& basePath,
                                        float rasterPixelHeight,
                                        UISystemContext& ctx,
                                        renderer::ResourceManager& resources)
{
    /// @note 静的アトラスは焼く解像度を持たない (PNG が決め打ち) ので、解像度をキーへ混ぜると
    /// @note 同じ PNG を段の数だけ読み込むだけになる。動的モードのときだけ分ける。
    const bool dynamicFont = renderer::FontAtlas::IsDynamicFontPath(basePath)
        || (basePath.starts_with("guid:")
            && renderer::FontAtlas::IsDynamicFontPath(ResolveUIFontBasePath(basePath)));
    const std::string key = dynamicFont
        ? basePath + '@' + std::to_string(static_cast<int>(rasterPixelHeight))
        : basePath;

    /// @note キャッシュは指定されたパスで引く。解決結果でキーを作ると、同じ指定が
    /// @note プロジェクト側と共有側で二重にロードされうる。
    auto it = ctx.fontAtlasCache.find(key);
    if (it != ctx.fontAtlasCache.end())
        return it->second;

    renderer::FontAtlas& atlas = ctx.fontAtlasCache[key];
    const std::string resolved = ResolveUIFontBasePath(basePath);
    if (!atlas.Load(resolved, resources, rasterPixelHeight)) {
        FBZZ_LOG_ERROR("UISystem: failed to load FontAtlas [%s]\n  resolved: %s",
                       basePath.c_str(), resolved.c_str());
    }

    return atlas;
}


/// @name テキスト描画（マルチドロー対応）
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
    const float fontSize = EffectiveFontSize(text);
    renderer::FontAtlas& atlas =
        GetOrLoadFontAtlas(text.fontPath, ResolveTextRasterPixelHeight(fontSize, ctx),
                           ctx, resources);
    if (!atlas.IsValid() || atlas.GetLineHeight() <= 0.0f) return;

    /// @note 描画経路からも焼いておく。UITextSizeSystem を通らないテキストでもグリフが要る。
    /// @note 登録済みなら UTF-8 走査だけで戻る。
    atlas.PrepareText(text.text, resources);

    const float baseScale = fontSize / atlas.GetLineHeight();

    /// @note 整列と改行位置の決定に行分割が要るので常に事前計算する。
    /// @note Left だけ別扱いにすると UTF-8 走査が 2 通りになり、そちらの方がズレの温床になる。
    std::vector<UITextLine>& lines = ctx.textLineScratch;
    LayoutTextLines(text, atlas, fontSize, lines);
    ApplyTextOverflow(text, lines);
    if (lines.empty()) return;

    /// @note 実測サイズは UITextSizeSystem が同じフレームの描画前に確定させている。
    /// @note 測るのは ComputeTextLogicalSize だけ。2 箇所で出すと描画位置と当たり判定がずれる。
    const math::Vector2 measured = (text.resolvedSize.x > 0.0f || text.resolvedSize.y > 0.0f)
        ? text.resolvedSize
        : math::Vector2{ 0.0f, lines.front().height };

    /// @note 矩形の左上。ここから先は画像とまったく同じ規則で置かれる。
    const UIRect box = ResolveUIRect(parentSize, position, measured, text.anchoring);

    /// @note 縦揃え。箱に余りがあるときだけ意味を持つ (余りが無ければ 0 になる)。
    const float contentHeight = TotalTextHeight(lines, text.lineSpacing);
    const float slack = (std::max)(0.0f, measured.y - contentHeight);
    const float originY = box.position.y
        + (text.verticalAlign == TextVerticalAlign::Middle ? slack * 0.5f
         : text.verticalAlign == TextVerticalAlign::Bottom ? slack : 0.0f);

    /// @note align は「確保した矩形の中で行をどちらへ寄せるか」だけを決める。
    /// @note position の意味まで変えると、中央揃えにした瞬間に文字が左へ半分ずれる。
    auto lineStartX = [&](float lineWidth) -> float {
        if (text.align == TextAlign::Center) return box.position.x + (measured.x - lineWidth) * 0.5f;
        if (text.align == TextAlign::Right)  return box.position.x + (measured.x - lineWidth);
        return box.position.x;
    };

    /// @note ページごとに頂点を分ける。BMFont のマルチページアトラスではグリフごとに参照
    /// @note テクスチャが変わるので、まとめてから分ければ切り替えが最小回数で済む。
    std::vector<std::vector<UIVertex2D>>& pageVerts = ctx.textPageScratch;
    if (pageVerts.size() < atlas.GetPageCount())
        pageVerts.resize(atlas.GetPageCount());
    if (pageVerts.empty()) return;
    const std::size_t pageCount = atlas.GetPageCount();
    for (std::size_t i = 0; i < pageCount; ++i) pageVerts[i].clear();
    pageVerts[0].reserve(text.text.size() * 6);

    /// @note グリフ 1 つを四角形として積む。マスクのクリップもここで通す。
    /// @note ドロー単位で切ると「はみ出した 1 文字のために行ごと消える」になる。
    /// @note 枠のところで文字が半分だけ見えるのが正しい。
    const auto emitGlyph = [&](const renderer::FontGlyph& g, float x, float y,
                               float x2, float y2, float shear,
                               const math::Vector4& color) {
        const std::size_t page =
            (g.page >= 0 && static_cast<std::size_t>(g.page) < pageVerts.size())
                ? static_cast<std::size_t>(g.page) : 0;
        std::vector<UIVertex2D>& verts = pageVerts[page];

        /// @note 疑似イタリックは下端を固定して上端をずらす。字が浮かないよう軸は足元。
        const ClipVertex quad[4] = {
            { { x  + shear, y  }, { g.u0, g.v0 }, color },
            { { x,          y2 }, { g.u0, g.v1 }, color },
            { { x2,         y2 }, { g.u1, g.v1 }, color },
            { { x2 + shear, y  }, { g.u1, g.v0 }, color },
        };
        ClipVertex clipped[kMaxClipPolygonVertices];
        const int count = ClipPolygonToPlanes(ctx.clipPlanes, quad, 4,
                                              clipped, kMaxClipPolygonVertices);
        if (count < 3) return;
        for (int t = 1; t + 1 < count; ++t) {
            verts.push_back({ clipped[0].local,     clipped[0].uv,     clipped[0].color });
            verts.push_back({ clipped[t].local,     clipped[t].uv,     clipped[t].color });
            verts.push_back({ clipped[t + 1].local, clipped[t + 1].uv, clipped[t + 1].color });
        }
    };

    /// @note 省略記号の送り幅。行末をどこで打ち切るかの判定に要る。
    const renderer::FontGlyph* ellipsisGlyph = atlas.GetGlyph(U'…');
    const float ellipsisAdvance = ellipsisGlyph
        ? ellipsisGlyph->advance * baseScale + text.letterSpacing : 0.0f;

    /// @note 描く文字数の上限。-1 は無制限。計測は全文で行う ─ 1 文字ずつ出す間に箱が
    /// @note 伸び縮みすると周りのレイアウトが毎フレーム動く。
    const int visibleLimit = text.visibleCharacters;
    int drawnCharacters = 0;

    /// @note 見た目の状態は行をまたいで続く。行ごとに作り直すと </color> が行頭で切れる。
    UIRichTextState style{};
    std::size_t cursor = 0;
    float penY = originY;

    for (std::size_t li = 0; li < lines.size(); ++li) {
        const UITextLine& line = lines[li];
        float penX = lineStartX(line.width);
        /// @note 省略する行は、記号ぶんの幅を残したところで打ち切る。
        /// @note 基準は箱の右端であって、行の開始位置ではない。
        const float lineLimitX = box.position.x + measured.x - ellipsisAdvance;

        char32_t previous = 0;
        cursor = line.begin;
        bool truncatedHere = false;

        while (cursor < line.end) {
            std::size_t charBegin = cursor;
            const char32_t code =
                NextRichGlyph(text.text, cursor, text.richText, fontSize, style, charBegin);
            if (code == 0 || code == U'\n') break;
            /// @note 行末がタグで終わっていると、走査はタグを食べたあと次の行の 1 文字目まで
            /// @note 読み進んでしまう。見た目の変化は残したまま、字だけをここで止める。
            if (charBegin >= line.end) { cursor = charBegin; break; }
            if (visibleLimit >= 0 && drawnCharacters >= visibleLimit) { truncatedHere = true; break; }

            const float glyphScale = baseScale * style.scale;
            if (previous != 0)
                penX += atlas.GetKerning(previous, code) * glyphScale;
            previous = code;

            const renderer::FontGlyph* g = atlas.GetGlyph(code);
            if (!g) {
                penX += atlas.GetFallbackAdvance() * glyphScale + text.letterSpacing;
                ++drawnCharacters;
                continue;
            }
            if (line.ellipsis && penX + g->advance * glyphScale > lineLimitX) {
                truncatedHere = true;
                break;
            }

            /// @note 空白文字のように画像を持たないグリフは、送りだけ進めて四角形を出さない。
            if (g->width > 0.0f && g->height > 0.0f) {
                /// @note 行の下端を揃える。`<size>` で大きい字が混ざっても足元が動かない。
                const float baselineShift = (line.height - fontSize * style.scale);
                const float x  = penX + g->xOffset * glyphScale;
                const float y  = penY + baselineShift + g->yOffset * glyphScale;
                const float x2 = x + g->width  * glyphScale;
                const float y2 = y + g->height * glyphScale;
                const float shear = style.italic ? (y2 - y) * kFauxItalicShear : 0.0f;

                emitGlyph(*g, x, y, x2, y2, shear, style.color);
                if (style.bold) {
                    const float nudge = fontSize * style.scale * kFauxBoldOffset;
                    emitGlyph(*g, x + nudge, y, x2 + nudge, y2, shear, style.color);
                }
            }

            penX += g->advance * glyphScale + text.letterSpacing;
            ++drawnCharacters;
        }

        if (line.ellipsis && truncatedHere && ellipsisGlyph
            && ellipsisGlyph->width > 0.0f && ellipsisGlyph->height > 0.0f) {
            const float baselineShift = line.height - fontSize;
            const float x  = penX + ellipsisGlyph->xOffset * baseScale;
            const float y  = penY + baselineShift + ellipsisGlyph->yOffset * baseScale;
            emitGlyph(*ellipsisGlyph, x, y,
                      x + ellipsisGlyph->width * baseScale,
                      y + ellipsisGlyph->height * baseScale, 0.0f, style.color);
        }

        penY += line.height * text.lineSpacing;
        if (visibleLimit >= 0 && drawnCharacters >= visibleLimit) break;
    }

    /// @note 定数バッファは全ページ・全チャンク共通なので 1 回だけ更新する。
    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = text.color;
    constants.uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    /// @note テキストは 1 ドローに複数グリフを詰めるため、矩形が 1 つに定まらない。
    /// @note 図形を描くための寸法は無い、という意味でゼロを渡す。
    constants.rect   = { 0.0f, 0.0f, 0.0f, 0.0f };
    resources.Update(ctx.constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    /// @note UIText.hlsl の .r チャンネルを coverage として使い、alpha チャンネル依存を排除する。
    call.shader             = ctx.textShader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = ctx.constants;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;

    /// @note 未設定 (大多数) なら nullptr が返り、以降は組み込みのテキスト描画になる。
    if (const UIMaterialBinding* material =
            ResolveUIMaterial(ctx, resources, text.materialPath);
        material && material->valid) {
        const renderer::Material& resolved = material->material;
        call.shader = resolved.shader;
        const auto materialPso = (pso == ctx.worldPso) ? material->worldPso : material->screenPso;
        if (materialPso.IsValid()) call.pipelineState = materialPso;
        call.constantBuffers[2] = resolved.paramsBuffer;
        /// @note t0 はフォントアトラスで UISystem がページごとに差し替える。albedo で潰すと
        /// @note 文字が消えるので、追加の絵は t1 以降 (tex5-tex7) を使うこと。
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

        /// @note kTextVBVertices を超えるテキストはチャンク分割して複数ドローで描く。
        /// @note 3 の倍数へ丸めるのは、4096 が 3 で割り切れず、そのまま切ると境目に
        /// @note 三角形 1 枚に足りない頂点が残って画面を横切る破片になるため。
        /// @note クリップで 1 文字あたりの頂点数が一定でなくなった今は短い文章でも当たりうる。
        constexpr uint32_t kTextChunkVertices = (kTextVBVertices / 3) * 3;
        uint32_t offset = 0;
        const uint32_t total = static_cast<uint32_t>(verts.size());
        while (offset < total) {
            const uint32_t chunk = (std::min)(total - offset, kTextChunkVertices);
            /// @note チャンクごとに別実体を借りる。1 本を上書きすると、記録型バックエンドでは
            /// @note 全チャンクが最後のグリフ群になる。
            const auto vertexBuffer = ctx.vertexPool.Acquire(
                resources, kTextVBVertices, static_cast<uint32_t>(sizeof(UIVertex)));
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
        /// @note resolvedSize は UITextSizeSystem が本体へ書き済み。複製は fontPath を
        /// @note 補うだけなので、書き戻すものは何も無い。
        SubmitTextWithAtlas(renderer, resources, ctx, canvasToClip, pso, layer,
                            defaulted, position, parentSize);
        return;
    }
    SubmitTextWithAtlas(renderer, resources, ctx, canvasToClip, pso, layer,
                        text, position, parentSize);
}


/// @note UIButton の状態別スプライトを、同じ GameObject の UIImage へ反映する。
/// @note 差し替えは「今どの状態か」の関数でしかないので描画側で当てる。イベント側で書くと、
/// @note 状態が変わらなかったフレームや Play していない Editor で絵が付かない。
void ApplyButtonSpriteSwap(UIImage& image, UIButton* button)
{
    /// @note 空欄の状態は差し替えない = UIImage に指定された絵のまま。
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

/// @note texturePath から GPU テクスチャと、Sprite サブアセットの切り出しを解決する。
/// @note テクスチャは非同期に読まれるので要求したフレームには寸法が無く、UV も Border も
/// @note 出せない。1 回きりの解決にするとその状態が固定されて絵が伸びたままになる。
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
    uint32_t sourceWidth = 0, sourceHeight = 0;
    image.texture = asset::StreamedTextureResolver::Engine().ResolveGpuWithSourceSize(
        resources, source, sourceWidth, sourceHeight);
    const float width = static_cast<float>(sourceWidth);
    const float height = static_cast<float>(sourceHeight);
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

/// @name デバッグ表示
/// @note 矩形・アンカー・ピボットを白テクスチャの細い四角で重ねる。
/// @note 描くのは RectFromTransform が返した矩形そのもの。別に計算した図だと、ずれたときに
/// @note 「表示のバグ」なのか「判定のバグ」なのか分からない。
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
    /// @note 上
    bar(position.x, position.y, size.x, thickness);
    /// @note 下
    bar(position.x, position.y + size.y - thickness, size.x, thickness);
    /// @note 左
    bar(position.x, position.y, thickness, size.y);
    /// @note 右
    bar(position.x + size.x - thickness, position.y, thickness, size.y);
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

/// @name レンダリング
/// @note 選択中の要素の矩形を、選択マスクへ白で塗る。
/// @note sortOrder を無視するのは、マスクが被覆だけを表すので重なりの前後が結果に出ないため。
/// @note 絵ではなく矩形を塗るのは、アルファでシルエットを取ると暗い絵や絵を持たない
/// @note Mask / Scroll View がマスクに出ないため。
void SubmitSelectionMaskRecursive(GameObject& go,
                                  const UITransform2D& parentTransform,
                                  renderer::IRenderer& renderer,
                                  renderer::ResourceManager& resources,
                                  UISystemContext& ctx,
                                  const math::Matrix4& canvasToClip,
                                  renderer::RenderLayer layer,
                                  const std::function<bool(GameObject&)>& isSelected,
                                  bool applySelfTransform)
{
    if (!go.activeInHierarchy()) return;

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;
    const Rect rect = RectFromTransform(go, resolved);

    /// @note Canvas 自身 (applySelfTransform == false) は塗らない。選ぶと画面全体が
    /// @note 囲われるだけで、どの要素を見ているのか分からなくなる。
    if (applySelfTransform && rect.size.x > 0.0f && rect.size.y > 0.0f && isSelected(go)) {
        SubmitImage(renderer, resources, ctx, canvasToClip, ctx.selectionMaskPso, layer,
                    rect.pos, rect.size, { 1.0f, 1.0f, 1.0f, 1.0f },
                    { 0.0f, 0.0f }, { 1.0f, 1.0f }, ctx.whiteTexture, resolved.rotationZ);
    }

    UITransform2D childTransform = resolved;
    /// @note Canvas ルートでは上書きしない (ProcessUIEventsRecursive と同じ理由)。
    if (applySelfTransform)
        childTransform.parentSize = rect.size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= RotateUIVector(scroll->scrollPosition, resolved.rotationZ);

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            SubmitSelectionMaskRecursive(*child, childTransform, renderer, resources, ctx,
                                         canvasToClip, layer, isSelected, true);
    }
}

/// @param groupAlpha 上位の UICanvasGroup を掛け合わせた透明度。
void RenderCanvasRecursive(GameObject& go,
                           const UITransform2D& parentTransform,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           UISystemContext& ctx,
                           const math::Matrix4& canvasToClip,
                           renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                           renderer::RenderLayer layer,
                           bool applySelfTransform = true,
                           std::size_t depth = 0,
                           float groupAlpha = 1.0f,
                           const asset::UIThemeStyle* theme = nullptr)
{
    if (!go.activeInHierarchy()) return;

    /// @note 群の透明度は掛け合わせる。入れ子のフェードが互いを打ち消さないため。
    if (const auto* group = go.GetComponent<UICanvasGroup>(); group && group->enabled) {
        groupAlpha = group->ignoreParentGroups
            ? std::clamp(group->alpha, 0.0f, 1.0f)
            : groupAlpha * std::clamp(group->alpha, 0.0f, 1.0f);
    }
    /// @note 完全に透明なら以下は 1 枚も出ない。子孫ごと降りずに済ませる。
    if (groupAlpha <= 0.0f) return;

    UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;
    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    auto* text   = go.GetComponent<UIText>();

    /// @note 押下中はボタン全体を右下へ沈ませる。pressedColor だけだと暗い背景との乗算後に
    /// @note 差が小さく、入力が UIButton まで届いたのかを画面で判別できない。
    if (button && button->enabled && button->isInteractable
        && button->state == UIButtonState::PRESSED) {
        constexpr math::Vector2 PRESSED_VISUAL_OFFSET = { 2.0f, 2.0f };
        resolved.position += RotateUIVector(PRESSED_VISUAL_OFFSET, resolved.rotationZ);
    }

    if (image && image->enabled) {
        ApplyButtonSpriteSwap(*image, button);
        ResolveImageTexture(*image, resources);

        const Rect    r     = RectFromTransform(go, resolved);
        math::Vector4 color = ResolveUIImageColor(*image, theme);
        if (button) {
            const auto* navigation = go.GetComponent<UINavigation>();
            color = ResolveUIButtonImageColor(color, *button, theme,
                navigation && navigation->enabled && navigation->focused);
        }
        /// @note 群の透明度はここで乗せる。コンポーネントの値は書き換えない ─
        /// @note 書き換えるとフェードの途中で保存したときに薄い色が焼き付く。
        color.w *= groupAlpha;

        const math::Vector2 uvMin = image->hasResolvedSprite
            ? image->resolvedSpriteUvMin : image->uvMin;
        const math::Vector2 uvMax = image->hasResolvedSprite
            ? image->resolvedSpriteUvMax : image->uvMax;
        const float fillAmount = std::clamp(image->fillAmount, 0.0f, 1.0f);

        if (r.size.x > 0.0f && r.size.y > 0.0f && fillAmount > 0.0f) {
            /// @note 未設定 (圧倒的多数) なら nullptr が返り、以降は従来どおりの経路になる。
            UIMaterialBinding* material =
                ResolveUIMaterial(ctx, resources, image->materialPath);
            const ImageDrawContext draw{
                renderer, resources, ctx, canvasToClip, pso, layer,
                r.pos, r.size,
                std::cosf(resolved.rotationZ), std::sinf(resolved.rotationZ),
                color,
                image->texture.IsValid() ? image->texture : ctx.whiteTexture,
                material,
                /// @note WorldSpace / ScreenSpaceCamera Canvas は深度テストありの PSO を使う。
                /// @note マテリアル側にも同じ使い分けの PSO があるので、どちらかを選ぶ。
                (pso == ctx.worldPso),
                image
            };

            /// @note 塗り潰しは描画時にセルを削るだけで、transform.scale もレイアウトも
            /// @note 当たり判定も動かさない (ゲージが減っても押せる範囲は変わらない)。
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
        if (theme) {
            UIText working = *text;
            ApplyUIThemeToText(*theme, working);
            working.color.w *= groupAlpha;
            SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, working,
                       resolved.position, resolved.parentSize);
        } else if (groupAlpha >= 1.0f) {
            SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, *text,
                       resolved.position, resolved.parentSize);
        } else {
            /// @note 群の透明度は「今このフレームどう見えるか」でしかない。color へ直接書くと、
            /// @note フェード中に Play を止めた値がシーンの差分として残る。
            UIText faded = *text;
            faded.color.w *= groupAlpha;
            SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, faded,
                       resolved.position, resolved.parentSize);
        }
    }

    /// @note 判定に使っているのと同じ矩形を重ねる。図と判定を別に計算しない。
    if (ctx.showRects) {
        const Rect debugRect = RectFromTransform(go, resolved);
        if (debugRect.size.x > 0.0f && debugRect.size.y > 0.0f) {
            const UIAnchor anchoring = UIElementAnchoring(go);
            SubmitDebugRect(renderer, resources, ctx, canvasToClip, pso, layer,
                            debugRect.pos, debugRect.size,
                            { 0.0f, 0.85f, 1.0f, 0.55f }, 1.0f);
            /// @note ピボット (自分のどこが基準点に合っているか)。Editor のギズモと同じ桃色。
            /// @note アンカーは親の矩形も一緒に見えていないと意味を持たないので実行時は描かない。
            /// @note 実行時に知りたいのは「判定に使われている矩形はどこか」だけ。
            SubmitDebugMarker(renderer, resources, ctx, canvasToClip, pso, layer,
                              { debugRect.pos.x + debugRect.size.x * anchoring.pivot.x,
                                debugRect.pos.y + debugRect.size.y * anchoring.pivot.y },
                              { 1.0f, 0.47f, 0.78f, 0.9f }, 3.0f);
        }
    }

    UITransform2D childTransform = resolved;
    /// @note 子のアンカーは「この要素の矩形」に対する割合になる。
    /// @note Canvas ルートでは上書きしない (ProcessUIEventsRecursive と同じ理由)。
    if (applySelfTransform)
        childTransform.parentSize = RectFromTransform(go, resolved).size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= RotateUIVector(scroll->scrollPosition, resolved.rotationZ);

    /// @note Mask / Scroll View はここから下の描画を自分の矩形で切る。
    /// @note 自分自身は切らない (枠の絵を切ると縁が半分消える)。切るのは子孫だけ。
    std::size_t pushedPlanes = 0;
    if (applySelfTransform && ElementClipsChildren(go)) {
        const Rect clipRect = RectFromTransform(go, resolved);
        if (clipRect.size.x > 0.0f && clipRect.size.y > 0.0f)
            pushedPlanes = PushClipRect(ctx.clipPlanes, clipRect, resolved.rotationZ);
    }

    /// @note 参照で受ける。この並びは子の再帰処理が終わるまで読み続けるので、深さごとの
    /// @note 領域を借りたまま降りる。コピーすると確保が戻ってくる。
    std::vector<GameObject*>& children = AcquireChildScratch(ctx, depth);
    SortUIChildren(go, children);
    for (std::size_t i = 0; i < children.size(); ++i)
        RenderCanvasRecursive(*children[i], childTransform, renderer, resources, ctx,
                              canvasToClip, pso, layer, true, depth + 1, groupAlpha, theme);

    ctx.clipPlanes.resize(ctx.clipPlanes.size() - pushedPlanes);
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

    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView, ctx.sceneDepthAvailable))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go,
            viewportWidth, viewportHeight,
            rawMouseInViewport, viewProjection,
            cameraWorldPos, cameraWorldRot, ctx, targetView);
        ctx.textPixelScale =
            ResolveCanvasPixelScale(*entry.canvas, viewportWidth, viewportHeight);
        const UITransform2D canvasRoot =
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
        const math::Matrix4 renderCanvasToClip = ctx.reversedDepth
            ? renderer::ReverseClipDepth(state.canvasToClip) : state.canvasToClip;
        RenderCanvasRecursive(*entry.go, canvasRoot, renderer, resources, ctx,
                              renderCanvasToClip, state.pso, state.layer, false, 0, 1.0f,
                              entry.theme ? &*entry.theme : nullptr);
    }
}

} ///< @note namespace (anonymous)


/// @name 公開 API
void UISystemSetDefaultFontPath(UISystemContext& ctx, const std::string& basePath)
{
    ctx.defaultFontPath = basePath;
}

void UISystemReleaseGpuResources(UISystemContext& ctx, renderer::ResourceManager&)
{
    UISystemReleaseGpuResources(ctx);
}

void UISystemReleaseGpuResources(UISystemContext& ctx)
{
    ctx.vertexPool.ReleaseAll();
    ReleaseUIHandle(ctx.constants);
    ReleaseUIHandle(ctx.pso);
    ReleaseUIHandle(ctx.worldPso);
    ReleaseUIHandle(ctx.selectionMaskPso);
    ReleaseUIHandle(ctx.depthCopyConstants);
    ReleaseUIHandle(ctx.depthCopyPso);
    ctx.shader = {};
    ctx.textShader = {};
    ctx.depthCopyShader = {};
    ctx.whiteTexture = {};
    ctx.initialized = false;
    ctx.cachedResourceOwner = 0;
    ctx.cachedResetVersion = 0;
    UISystemFlushCache(ctx, nullptr);
}

bool UICopySceneDepth(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                      UISystemContext& ctx, renderer::ResourceHandle<renderer::TextureTag> sceneDepth,
                      bool reversedOutput)
{
    ctx.reversedDepth = reversedOutput;
    if (!sceneDepth.IsValid()) return false;
    SyncCacheGenerations(ctx, resources);
    if (!ctx.depthCopyShader.IsValid()) ctx.depthCopyShader = resources.LoadShader("Assets/Shaders/UI/UIDepthCopy.hlsl");
    if (!ctx.depthCopyConstants.IsValid()) ctx.depthCopyConstants = resources.CreateConstantBuffer(sizeof(math::Vector4));
    if (!ctx.depthCopyPso.IsValid()) ctx.depthCopyPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
    if (!ctx.depthCopyShader.IsValid() || !ctx.depthCopyConstants.IsValid() || !ctx.depthCopyPso.IsValid()) return false;
    const math::Vector4 options{reversedOutput ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    resources.Update(ctx.depthCopyConstants, &options, sizeof(options));
    renderer.ClearDepth();
    renderer::DrawCall call;
    call.shader = ctx.depthCopyShader;
    call.pipelineState = ctx.depthCopyPso;
    call.vertexCount = 3;
    call.constantBuffers[0] = ctx.depthCopyConstants;
    call.textures[7] = sceneDepth;
    renderer.Submit(call, resources);
    return true;
}


void UISystemFlushCache(UISystemContext& ctx, renderer::ResourceManager*)
{
    ReleaseMaterialCache(ctx);
    /// @note アトラスのページも記録済みの作成元へ返す。Reset 後の古い世代は資源プールが拒否する。
    ctx.fontAtlasCache.clear();
    /// @note 子リストの作業領域は破棄済み GameObject を指したままになりうる。
    /// @note 中身だけ捨てて、確保済みの容量は次のシーンでそのまま使い回す。
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
    if (targetView == UIRenderTargetView::GameViewport) {
        UpdateUIInteraction(scene, ctx, {viewportWidth, viewportHeight, mouseInViewport,
            viewProjection, cameraWorldPos, cameraWorldRot, ctx.gameplayInput, ctx.sceneDepthAvailable}, mousePressed);
    } else UpdateUILayout(scene, ctx, viewportWidth, viewportHeight);
    SyncCacheGenerations(ctx, resources);
    EnsureInit(ctx, resources);
    BeginUIFrame(ctx);

    if (!ctx.shader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() || !ctx.whiteTexture.IsValid())
        return;

    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    UIRenderSystem(canvases, renderer, resources, ctx,
                   viewportWidth, viewportHeight, mouseInViewport,
                   cameraWorldPos, cameraWorldRot, viewProjection, targetView);
}

void UISelectionMaskSystem(Scene& scene,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           UISystemContext& ctx,
                           float viewportWidth,
                           float viewportHeight,
                           const std::function<bool(GameObject&)>& isSelected,
                           math::Vector3    cameraWorldPos,
                           math::Quaternion cameraWorldRot,
                           const math::Matrix4& viewProjection,
                           UIRenderTargetView targetView)
{
    if (!isSelected) return;

    SyncCacheGenerations(ctx, resources);
    EnsureInit(ctx, resources);
    BeginUIFrame(ctx);

    if (!ctx.shader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.selectionMaskPso.IsValid() || !ctx.whiteTexture.IsValid())
        return;

    /// @note レイアウトはまだ本描画の前なので、この時点の transform で解く。
    /// @note UILayoutGroup も UIText の実測も前フレームぶんが載っており、1 フレーム古い矩形に
    /// @note なるのは動いている最中だけ。
    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go, viewportWidth, viewportHeight,
            math::Vector2::ZERO, viewProjection,
            cameraWorldPos, cameraWorldRot, ctx, targetView);

        const UITransform2D canvasRoot =
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
        /// @note 選択マスク RT は Reversed-Z。スクリーン空間 (z=0) は最手前の 1 へ、
        /// @note ワールド空間はカメラの GPU 射影と同じ深度へ移る。
        const math::Matrix4 canvasToClip = renderer::ReverseClipDepth(state.canvasToClip);
        SubmitSelectionMaskRecursive(*entry.go, canvasRoot, renderer, resources, ctx,
                                     canvasToClip, state.layer, isSelected, false);
    }
}

} ///< @note namespace fbzz::scene
