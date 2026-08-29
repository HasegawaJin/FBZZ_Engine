/// @file    UISystem.cpp
/// @brief   ランタイム UI の描画とボタン入力処理。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// UICanvas / UIImage / UIText / UIButton を走査し、DrawCall と hit 状態を作る。
/// ScreenSpace / ScreenSpaceCamera / WorldSpace の 3 モードを扱う。
#include "Engine/Scene/Systems/UISystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/UICanvas.hpp"
#include "Engine/Scene/Components/UICanvasGroup.hpp"
#include "Engine/Scene/Components/UIImage.hpp"
#include "Engine/Scene/Components/UIButton.hpp"
#include "Engine/Scene/Components/UIText.hpp"
#include "Engine/Scene/UIPointer.hpp"
#include "Engine/Scene/Components/UILayoutGroup.hpp"
#include "Engine/Scene/Components/UIControls.hpp"
#include "Engine/Scene/Components/UIElement.hpp"
#include "Engine/Input/Input.hpp"
#include "Engine/Input/Gamepad.hpp"
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
#include <string>
#include <string_view>
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
    // 角丸・枠線・影は「何ピクセルぶん」で決まる。UV だけだと縦横比で角の形が変わる。
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

// 頂点バッファの固定容量。
// 矩形は 6 頂点で足りるが、Radial の扇形切りで 24 まで太り、Mask / Scroll View の
// クリップは半平面 1 枚につき頂点を 1 つ増やす。借用制のバッファは後から広げられない
// ので最大側で確保する。64 は 3 段の入れ子 (12 枚) までは切り落とされない数。
static constexpr uint32_t kImageVBVertices = 64;
// 1 つの多角形を切るときの作業配列の長さ。上と同じ理由で余裕を持たせる。
static constexpr int kMaxClipPolygonVertices = 32;
static constexpr uint32_t kTextVBVertices  = 4096; // ~682 グリフ分。超過時は複数ドローに分割する

struct Rect { math::Vector2 pos; math::Vector2 size; };

// ── 多角形の切り取り ──────────────────────────────────────────────────────────
// 扇形の塗り潰しも Mask / Scroll View のクリップも「凸多角形を半平面で削る」でしかない。
struct ClipVertex {
    math::Vector2 local;   // Canvas 空間、または要素ローカル (使う側で揃える)
    math::Vector2 uv;      // セル内 0..1 またはアトラス UV
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// dot(p - point, normal) >= 0 側を残す (Sutherland-Hodgman)。
// 凸多角形の入力に対し、出力は入力 + 1 頂点までしか増えない。
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

// いま積まれているクリップ面すべてで多角形を削る。頂点は Canvas 空間で渡すこと。
// 何も残らなければ 0。1 枚ずつ順に当てるのは、共通部分の定義がそのまま
// 「全半平面を満たす点」だから。矩形どうしの交差として畳むと回転で表せなくなる。
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

// 矩形 (回転あり) を内向き 4 面としてクリップスタックへ積む。
// 戻り値は積んだ枚数。抜けるときに同じ数だけ外す。
std::size_t PushClipRect(std::vector<UIClipPlane>& planes, const Rect& rect, float rotationZ)
{
    const float c = std::cosf(rotationZ);
    const float s = std::sinf(rotationZ);
    // 回転の中心は要素の中心。描画側 (LocalToCanvas) と同じ規則で揃える。
    const math::Vector2 center = { rect.pos.x + rect.size.x * 0.5f,
                                   rect.pos.y + rect.size.y * 0.5f };
    // 辺の外向き法線を回した 4 本。内側に残したいので符号を反転して積む。
    const math::Vector2 axisX = { c, s };    // 要素ローカルの +x が Canvas 上で向く方向
    const math::Vector2 axisY = { -s, c };   // 同 +y
    const float halfW = rect.size.x * 0.5f;
    const float halfH = rect.size.y * 0.5f;

    const auto push = [&](const math::Vector2& normal, float halfExtent) {
        // 面上の 1 点 = 中心から法線と逆向きへ halfExtent 進んだところ。
        planes.push_back({ { center.x - normal.x * halfExtent,
                             center.y - normal.y * halfExtent }, normal });
    };
    push(axisX, halfW);                      // 左辺 (内向き = +x)
    push({ -axisX.x, -axisX.y }, halfW);     // 右辺
    push(axisY, halfH);                      // 上辺 (内向き = +y)
    push({ -axisY.x, -axisY.y }, halfH);     // 下辺
    return 4;
}

// この要素がクリップを張るか。Mask と Scroll View のどちらも中身を枠で切る。
// Scroll View も切るのは、切らないと枠の外へはみ出したぶんが見えてしまうため。
bool ElementClipsChildren(GameObject& go)
{
    if (const auto* mask = go.GetComponent<UIMask>(); mask && mask->enabled && mask->affectChildren)
        return true;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        return true;
    return false;
}

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
// 出力引数なのは UI ノード 1 つにつき毎フレーム呼ばれるため (vector を返すと
// 確保と解放が規模 × フレームレートで走り続ける)。
// 並びは子の処理中も読み続けるので、呼び出し側は深さごとに別の領域を渡すこと
// (AcquireChildScratch を参照)。
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

// 再帰の深さごとに 1 本ずつ領域を貸す。1 本を共有すると最初の子を降りた先で
// 親のリストが上書きされ、2 番目以降の兄弟が消える。
// 返す参照はより深い階層で再度呼ばれても生き続ける
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
    // 親の位置と回転だけ継承し、サイズは各要素の localScale.xy から読む。
    // UI の localScale.xy は倍率ではなく幅・高さなので、TransformSystem のように
    // 親 scale を子 position へ掛けると親のサイズ変更だけで子が飛ぶ。
    const math::Vector2 localPos = { local.position.x, local.position.y };
    UITransform2D result{};
    result.position = parent.position + Rotate2D(localPos, parent.rotationZ);
    result.rotationZ = parent.rotationZ + ExtractZRotation(local.rotation);
    // アンカー / ピボットの解釈は ResolveUIRect に閉じる。
    // ここが運ぶのは「子から見た親のサイズ」だけ。
    result.parentSize = parentSize;
    return result;
}

// UI 要素の矩形。アンカー・ピボットの解釈は Components/UIRect.hpp が唯一の定義。
// size を引数で受けるのは、画像は scale.xy そのままだが文字は実測が要るため。
Rect ResolveElementRect(const UITransform2D& resolved,
                        const math::Vector2& size,
                        const UIAnchor& anchoring)
{
    const UIRect rect = ResolveUIRect(resolved.parentSize,
                                      resolved.position, size, anchoring);
    return { rect.position, rect.size };
}

// UI 要素の矩形サイズ。矩形の解決・当たり判定・レイアウトの 3 箇所が必要とする。
// 画像は transform.scale.xy が正、文字は実測 (UIText::resolvedSize) が正で
// scale はその写し。どちらを見るかを決めるのはここだけにする。
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
// Matrix4 に operator*(Vector4) が無いので、WorldSpace のヒット判定用にここで持つ。
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
// メッシュ側の SyncMaterial は MaterialComponent を入口に持つので、同じ入口に載せると
// UIImage 全部へ MaterialComponent を要求することになる (UI に submesh は無く、
// .mat を持たない要素が多数)。共有するのは入口ではなく「.mat をシェーダーへ束縛する
// 規則」の方で、そちらは Engine/Asset/MaterialParamBinding.hpp にある。
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

    // メッシュ用の .mat は b0 を CameraConstants として読むが、UI パスはそこへ ortho を
    // 入れる。割り当てると頂点が飛ぶか真っ黒になり、絵からは原因が分からない。
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
    // 共有側の paramData は触らない (同じ .mat を使う他の要素へ波及する)。
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
    // 選択マスク: 塗るのは被覆だけなので不透明。深度は書き込む (UISystem.hpp の WHY)。
    ctx.selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
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

// 頂点バッファの貸し出しを巻き戻す。1 フレームに選択マスクと本描画の 2 回入るので、
// フレームが変わったときだけ巻き戻して実体の衝突を防ぐ。
void BeginUIFrame(UISystemContext& ctx)
{
    // クリップは階層を降りるあいだだけ積むので、抜けれ切れば空になる。
    // それでも入口で均すのは、途中で return した経路が面を残さないため。
    ctx.clipPlanes.clear();
    if (ctx.lastResetFrame == fbzz::Time::frameCount) return;
    ctx.lastResetFrame    = fbzz::Time::frameCount;
    ctx.imageVertexCursor = 0;
    ctx.textVertexCursor  = 0;
}

// この DrawCall 専用の頂点バッファを 1 本借りる。足りなければ増やし、以降は使い回す。
// 記録型バックエンドでは「Draw ごとに別実体」が正しさの条件 (UISystemContext 参照)。
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

// Canvas 直下の子から見た「親の矩形」の寸法。
// Overlay の ScaleWithScreenSize では見えている範囲が viewport / scale で決まり、
// canvasHeight と一致しない。ここがずれると anchor = 1 が画面端に来ない
// (Editor の GetCanvasEditorSize() も最初からこの値で描いている)。
// WorldSpace と ScreenSpaceCamera は板を 3D へ置いたものなので画面の広さと無関係。
math::Vector2 ResolveCanvasRectSize(const UICanvas& canvas,
                                    float viewportWidth,
                                    float viewportHeight)
{
    if (canvas.renderMode != UIRenderMode::ScreenSpaceOverlay) {
        return { (std::max)(1.0f, canvas.canvasWidth),
                 (std::max)(1.0f, canvas.canvasHeight) };
    }
    float visibleW = 1.0f, visibleH = 1.0f;
    ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, visibleW, visibleH);
    return { visibleW, visibleH };
}

// Canvas 直下の子から見た「親」。セーフエリアぶんだけ内側へ寄せた矩形になる。
// 原点もずらすのは、寸法を縮めるだけだと左上に貼った要素が画面の角に残るため。
// 入力・描画・レイアウト・選択マスクの 4 経路が同じ矩形を要るので、作るのはここだけ。
UITransform2D BuildCanvasRootTransform(const UICanvas& canvas,
                                       float viewportWidth, float viewportHeight)
{
    const math::Vector2 full = ResolveCanvasRectSize(canvas, viewportWidth, viewportHeight);
    UITransform2D root{};
    root.position   = { canvas.safeArea.x, canvas.safeArea.y };
    root.parentSize = {
        (std::max)(0.0f, full.x - canvas.safeArea.x - canvas.safeArea.z),
        (std::max)(0.0f, full.y - canvas.safeArea.y - canvas.safeArea.w)
    };
    return root;
}

// Canvas 1px が実画面で何 px になるか。フォントを焼く解像度がこれで決まる。
// Canvas Scaler の倍率は ConstantPixelSize で常に 1 だが、1920x1080 の Canvas を
// 900px 幅へ映せば実際は 0.47 倍。「見えている領域」と viewport の比なら両モードを
// 同じ式で扱える。WorldSpace を 1 に固定するのは、実寸がカメラ距離で毎フレーム変わり
// グリフを焼き直し続けるため。
float ResolveCanvasPixelScale(const UICanvas& canvas, float viewportWidth, float viewportHeight)
{
    if (canvas.renderMode == UIRenderMode::WorldSpace) return 1.0f;

    float visibleW = 1.0f, visibleH = 1.0f;
    ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, visibleW, visibleH);
    return (std::max)((std::max)(1.0f, viewportWidth)  / visibleW,
                      (std::max)(1.0f, viewportHeight) / visibleH);
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

        // Canvas が子 GO に置かれることがあるので worldPosition を使う。
        // faceCamera は向きだけカメラ姿勢で上書きする (位置は追従したまま正対する)。
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
        // Canvas をカメラ前方 planeDistance に置いてカメラ向きで固定する。
        // Overlay と同じスクリーン UI に深度テストを足す用途。マウス座標系は Overlay と同じ。
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

    // 単純な乗算だと UIImage と UIButton の両方が暗色のとき Pressed Color が黒へ潰れる。
    // normalColor を基準に相対変換すれば、指定した色がそのまま画面へ出る。
    return {
        relativeChannel(imageColor.x, button.normalColor.x, targetColor.x),
        relativeChannel(imageColor.y, button.normalColor.y, targetColor.y),
        relativeChannel(imageColor.z, button.normalColor.z, targetColor.z),
        relativeChannel(imageColor.w, button.normalColor.w, targetColor.w)
    };
}

// ── 描画サブミット ────────────────────────────────────────────────────────────
// 組み上がった三角形リストを 1 ドローとして積む。頂点を呼び出し側から受け取るのは、
// 矩形しか描けない形にすると Radial の凸多角形が別実装になり、マテリアル適用と
// 定数バッファの組み立てが二重になるため。
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
        // 図形をシェーダーで描く UI マテリアルは albedo を持たないことが多い。
        // 無条件に上書きすると UIImage 側で指定した絵が黙って消える。
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

// 凸多角形を 1 枚。いま積まれているクリップ面で削ってから三角形へ開く。
// 全経路をここへ通さないと「マスクの中で 9-slice だけはみ出す」形の穴になる。
// 矩形・扇形・グリフの違いは多角形を作るところまでで、そこから先は同じ。
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

    // 扇状に開く。凸多角形なので、どの頂点を軸にしても裏返らない。
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

    // 頂点 UV は 0..1。アトラス UV を直接積むと頂点シェーダーの g_UVRect と二重写像になり、
    // 切り出し矩形を指定したとき内側の一部しかサンプリングされない。
    // この 0..1 は角丸などの図形を描くための矩形内座標も兼ねる。
    // 巻き順は左上 → 左下 → 右下 → 右上。切り取りが凸多角形前提なので 4 頂点で渡す。
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

// ── 画像セル ──────────────────────────────────────────────────────────────────
// 9-slice もタイルも「元画像の一部を要素の一部へ貼る」の繰り返し。セルへ揃えることで
// 塗り潰しのクリップを 1 か所に書けば Simple / Sliced / Tiled 全部に効く。
// pos は要素ローカル (左上原点・回転前)。回転は描画直前にまとめて掛ける。
struct ImageCell {
    math::Vector2 pos;
    math::Vector2 size;
    math::Vector2 uvMin;
    math::Vector2 uvMax;
};

// 1 要素を描くのに要る、セル間で変わらない値。セル単位の関数へ 14 個の引数を並べると
// 順番違いが型で止まらないので束ねる。
// メンバー名 renderer が名前空間 renderer と重なるため、型は fbzz:: から書く。
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
// セルごとに回すと 9-slice の四隅がばらばらに回って継ぎ目が開く。
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

// 9-slice の 1 軸ぶんの境界 (4 本) と、対応する UV を作る。
// multiplier が描画側だけを拡縮するので、元画像側と描画側は比例しない。
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
// 数ピクセルの素材を全画面へ敷くと 1 要素で数万ドローになるので上限を置き、
// 当たったらタイルを引き伸ばす。密度はずれるが隙間が空くよりは意図が伝わる。
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
// 180 度以下の扇形は「中心を通る 2 つの半平面の共通部分」なので、セルをその 2 枚で
// 切れば残りがそのまま描く形になる。近似ではないのでどの角度でも輪郭が正確に出る。
// 道具 (ClipVertex / ClipHalfPlane) はファイル先頭で定義済み。

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

// 90 / 180 は「四半円 / 半円で要素をちょうど覆う」形。中心を軸にしたまま角度だけ
// 絞ると要素の一部しか覆えないので、軸を角 (90) や辺の中点 (180) へ移す。
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

    // 隣り合う区画は 1 本の境界を共有する。区画ごとに角度を足し引きすると
    // 丸め誤差で 2 本に割れ、境目に隙間か二重塗りの筋が出る。
    float bounds[3] = {};
    for (int i = 0; i <= chunkCount; ++i) {
        const float offset = chunk * static_cast<float>(i);
        bounds[i] = clockwise ? base + offset : base - offset;
    }

    // WHY 区画ごとに submit するか: 2 区画をまとめて 1 つの多角形にはできない
    //     (合わせると凹むことがある)。区画は最大 2 つなので、ドローは高々 2 本。
    for (int i = 0; i < chunkCount; ++i) {
        // 切り取りは常に「角度の小さい側 → 大きい側」で渡す。
        const float low  = clockwise ? bounds[i]     : bounds[i + 1];
        const float high = clockwise ? bounds[i + 1] : bounds[i];
        const math::Vector2 dirLow  = { std::cosf(low),  std::sinf(low)  };
        const math::Vector2 dirHigh = { std::cosf(high), std::sinf(high) };

        // 内側の条件は cross(dirLow, v) >= 0 かつ cross(dirHigh, v) <= 0。
        // cross(d, v) は dot(v, (-d.y, d.x)) と同じなので、法線 2 本に直せる。
        ClipVertex work[kMaxClipPolygonVertices];
        ClipVertex sectorPoly[kMaxClipPolygonVertices];
        int n = ClipHalfPlane(polygon, 4, apex, { -dirLow.y, dirLow.x },
                              work, kMaxClipPolygonVertices);
        n = ClipHalfPlane(work, n, apex, { dirHigh.y, -dirHigh.x },
                          sectorPoly, kMaxClipPolygonVertices);
        if (n < 3) continue;

        // 扇形は要素ローカルで解いてある。Mask のクリップは Canvas 空間なので、
        // 渡す前にここで写す (2 つの座標系を 1 つの関数に混ぜない)。
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

// ── フォントアトラス ──────────────────────────────────────────────────────────
// フォントの実体パスを解決する。FontAtlas は Renderer 層にあり「プロジェクトの Assets →
// エンジン共有 Assets」の二段構えを知らないので、既定フォントを自前で持たない
// プロジェクトでは共有側の Roboto へ辿り着けず文字が 1 つも出ない。
// .fnt を付けてから解決するのは、拡張子なしだと AssetManager の実在チェックが
// 必ず外れてフォールバックが働かないため。
std::string ResolveFontBasePath(const std::string& basePath)
{
    if (basePath.empty()) return basePath;

    // .ttf / .otf / .ttc は動的モードで、パスがそのまま実体を指す。
    if (renderer::FontAtlas::IsDynamicFontPath(basePath))
        return asset::AssetManager::ResolveAssetPath(basePath);

    const std::string fntExt = ".fnt";
    const std::string resolved = asset::AssetManager::ResolveAssetPath(basePath + fntExt);
    if (resolved.size() > fntExt.size() && resolved.ends_with(fntExt))
        return resolved.substr(0, resolved.size() - fntExt.size());
    return basePath;
}

// このテキストを実画面で何 px の em として焼けばよいか。
// fontSize は Canvas 空間の行高さなので、Canvas → 実画面の倍率を掛けて実寸へ直す。
float ResolveTextRasterPixelHeight(float fontSize, const UISystemContext& ctx)
{
    return renderer::FontAtlas::ResolveRasterPixelHeight(fontSize * ctx.textPixelScale);
}

// 実際に使う文字サイズ。autoSize が解いた値があればそれを、無ければ指定値。
// 計測・アトラス解像度・描画の 3 箇所が同じ値を要る。1 つでも text.fontSize を
// 直接読むと、縮んだときだけ字が滲む。
float EffectiveFontSize(const UIText& text)
{
    return text.resolvedFontSize > 0.0f ? text.resolvedFontSize : text.fontSize;
}

// ── リッチテキスト ────────────────────────────────────────────────────────────
// タグは「文字を出さずに以降の見た目を変える」指示なので、走査の途中で畳めば
// 行分割も描画も「文字と属性の並び」だけを見ればよくなる。
// 入れ子に上限を置くのは、</color> の閉じ忘れで状態が積み上がり続けないため。
constexpr int kRichTextStackDepth = 8;
// 疑似ボールドのずらし量と、疑似イタリックの傾き (どちらも em 比)。
constexpr float kFauxBoldOffset = 0.035f;
constexpr float kFauxItalicShear = 0.21f;

struct UIRichTextState {
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    float scale  = 1.0f;   ///< 基準 fontSize に対する倍率
    bool  bold   = false;
    bool  italic = false;

    math::Vector4 colorStack[kRichTextStackDepth]{};
    float         scaleStack[kRichTextStackDepth]{};
    int colorDepth = 0;
    int scaleDepth = 0;
};

// "#RRGGBB" / "#RRGGBBAA" / "RRGGBB" を色へ。読めなければ false。
bool ParseRichColor(std::string_view body, math::Vector4& out)
{
    if (!body.empty() && body.front() == '#') body.remove_prefix(1);
    if (body.size() != 6 && body.size() != 8) return false;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    float channels[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (std::size_t i = 0; i + 1 < body.size(); i += 2) {
        const int hi = hex(body[i]);
        const int lo = hex(body[i + 1]);
        if (hi < 0 || lo < 0) return false;
        channels[i / 2] = static_cast<float>(hi * 16 + lo) / 255.0f;
    }
    out = { channels[0], channels[1], channels[2], channels[3] };
    return true;
}

// タグ 1 つを状態へ畳む。認識できないタグは false (呼び出し側が普通の文字として扱う)。
bool ApplyRichTag(std::string_view tag, float baseFontSize, UIRichTextState& state,
                  bool& outLineBreak)
{
    outLineBreak = false;
    if (tag.empty()) return false;

    if (tag == "br") { outLineBreak = true; return true; }
    if (tag == "b")  { state.bold = true;   return true; }
    if (tag == "/b") { state.bold = false;  return true; }
    if (tag == "i")  { state.italic = true;  return true; }
    if (tag == "/i") { state.italic = false; return true; }

    if (tag == "/color") {
        if (state.colorDepth > 0) state.color = state.colorStack[--state.colorDepth];
        else                      state.color = { 1.0f, 1.0f, 1.0f, 1.0f };
        return true;
    }
    if (tag == "/size") {
        if (state.scaleDepth > 0) state.scale = state.scaleStack[--state.scaleDepth];
        else                      state.scale = 1.0f;
        return true;
    }

    const std::size_t equals = tag.find('=');
    if (equals == std::string_view::npos) return false;
    const std::string_view name  = tag.substr(0, equals);
    const std::string_view value = tag.substr(equals + 1);

    if (name == "color") {
        math::Vector4 parsed{};
        if (!ParseRichColor(value, parsed)) return false;
        if (state.colorDepth < kRichTextStackDepth)
            state.colorStack[state.colorDepth++] = state.color;
        state.color = parsed;
        return true;
    }
    if (name == "alpha") {
        math::Vector4 parsed{};
        // alpha は 1 バイトだけ。色と同じ読み取りへ寄せるため 6 桁へ水増しする。
        std::string_view digits = value;
        if (!digits.empty() && digits.front() == '#') digits.remove_prefix(1);
        if (digits.size() != 2) return false;
        const std::string expanded = std::string("0000") + std::string(digits);
        if (!ParseRichColor(expanded, parsed)) return false;
        // 展開した 3 バイト目が alpha 値になる。
        state.color.w = parsed.z;
        return true;
    }
    if (name == "size") {
        if (baseFontSize <= 0.0f || value.empty()) return false;
        // std::stof は読めない文字列で例外を投げる。タグは手書きで誤記が普通に混ざるので、
        // 「読めなければ普通の文字として出す」で受け止める。
        std::size_t cursor = 0;
        const bool relative = value[0] == '+' || value[0] == '-';
        const float sign = value[0] == '-' ? -1.0f : 1.0f;
        if (relative) cursor = 1;
        float magnitude = 0.0f;
        bool  anyDigit  = false;
        for (; cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9'; ++cursor) {
            magnitude = magnitude * 10.0f + static_cast<float>(value[cursor] - '0');
            anyDigit  = true;
        }
        if (cursor < value.size() && value[cursor] == '.') {
            ++cursor;
            float place = 0.1f;
            for (; cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9'; ++cursor) {
                magnitude += static_cast<float>(value[cursor] - '0') * place;
                place *= 0.1f;
                anyDigit = true;
            }
        }
        if (!anyDigit || cursor != value.size()) return false;

        const float target   = sign * magnitude;
        const float absolute = relative ? baseFontSize + target : target;
        if (absolute <= 0.0f) return false;
        if (state.scaleDepth < kRichTextStackDepth)
            state.scaleStack[state.scaleDepth++] = state.scale;
        state.scale = absolute / baseFontSize;
        return true;
    }
    return false;
}

// 次に描く文字を 1 つ返す。タグはここで消化して state へ畳む。
// 戻り値 0 で終端。改行は U'\n' で返す (<br> も同じ値へ畳む)。
// 計測と描画で同じ関数を使う。タグの解釈が 2 箇所にあると「測った幅と描いた幅が違う」。
char32_t NextRichGlyph(const std::string& source, std::size_t& offset, bool richText,
                       float baseFontSize, UIRichTextState& state, std::size_t& outBegin)
{
    while (offset < source.size()) {
        outBegin = offset;
        if (richText && source[offset] == '<') {
            const std::size_t close = source.find('>', offset + 1);
            // 閉じない '<' は普通の文字。行末の不等号を消してしまわない。
            if (close != std::string::npos) {
                const std::string_view tag(source.data() + offset + 1, close - offset - 1);
                bool lineBreak = false;
                if (ApplyRichTag(tag, baseFontSize, state, lineBreak)) {
                    offset = close + 1;
                    if (lineBreak) return U'\n';
                    continue;   // 見た目を変えただけ。次の文字を探しに戻る。
                }
            }
        }
        return util::Utf8::Decode(source, offset);
    }
    return 0;
}

renderer::FontAtlas& GetOrLoadFontAtlas(const std::string& basePath,
                                        float rasterPixelHeight,
                                        UISystemContext& ctx,
                                        renderer::ResourceManager& resources)
{
    // 静的アトラスは焼く解像度を持たない (PNG が決め打ち) ので、解像度をキーへ混ぜると
    // 同じ PNG を段の数だけ読み込むだけになる。動的モードのときだけ分ける。
    const std::string key = renderer::FontAtlas::IsDynamicFontPath(basePath)
        ? basePath + '@' + std::to_string(static_cast<int>(rasterPixelHeight))
        : basePath;

    // キャッシュは指定されたパスで引く。解決結果でキーを作ると、同じ指定が
    // プロジェクト側と共有側で二重にロードされうる。
    auto it = ctx.fontAtlasCache.find(key);
    if (it != ctx.fontAtlasCache.end())
        return it->second;

    renderer::FontAtlas& atlas = ctx.fontAtlasCache[key];
    const std::string resolved = ResolveFontBasePath(basePath);
    if (!atlas.Load(resolved, resources, rasterPixelHeight)) {
        FBZZ_LOG_ERROR("UISystem: failed to load FontAtlas [%s]\n  resolved: %s",
                       basePath.c_str(), resolved.c_str());
    }

    return atlas;
}

// 文字列を行へ分割し、各行のバイト範囲と表示幅 (スケール適用済み) を out へ書く
// (空文字列でも 1 行)。レイアウト計算と描画が同じ送り幅の規則を要るので 1 本化する。
// バイト範囲まで返すのは、折り返しが元の文字列に改行文字の無い位置で行を切るため。
// 描く側が改行文字だけを見ると、測った行数と描く行数が食い違う。
// @param fontSize 試したい文字サイズ。autoSize の探索が別の値で呼び直すため、
//        text.fontSize ではなく引数で受ける。
void LayoutTextLines(const UIText& text,
                     const renderer::FontAtlas& atlas,
                     float fontSize,
                     std::vector<UITextLine>& out)
{
    out.clear();
    if (atlas.GetLineHeight() <= 0.0f) return;

    const float baseScale = fontSize / atlas.GetLineHeight();
    const float cellH     = fontSize;
    const float limit     = text.maxWidth > 0.0f ? text.maxWidth : 0.0f;

    std::size_t lineBegin = 0;      // 今の行の先頭バイト
    float       lineW     = 0.0f;
    // その行でいちばん大きい字の倍率。<size> を使うと行の高さが変わる。
    float       lineScale = 1.0f;
    char32_t    previous  = 0;

    // 直近の折り返し候補 (空白の直後)。単語の途中で切らないために覚えておく。
    bool        hasBreak    = false;
    std::size_t breakEnd    = 0;    // candidate の行終端 (空白は含めない)
    std::size_t breakNext   = 0;    // 次の行の先頭
    float       breakWidth  = 0.0f;

    UIRichTextState style{};
    // 折り返しで巻き戻すときのために、行頭時点の見た目も控える。
    UIRichTextState breakStyle{};

    auto pushLine = [&](std::size_t end, float width) {
        UITextLine line{};
        line.begin  = lineBegin;
        line.end    = end;
        line.width  = width;
        line.height = cellH * lineScale;
        out.push_back(line);
        lineScale = 1.0f;
    };

    std::size_t offset = 0;
    while (offset < text.text.size()) {
        std::size_t charBegin = offset;
        const char32_t code =
            NextRichGlyph(text.text, offset, text.richText, fontSize, style, charBegin);
        if (code == 0) break;

        if (code == U'\n') {
            pushLine(charBegin, lineW);
            lineBegin = offset;
            lineW     = 0.0f;
            previous  = 0;
            hasBreak  = false;
            continue;
        }

        const float glyphScale = baseScale * style.scale;

        // カーニングは「前の文字との組」に対して定義されるため、行頭では適用しない。
        const float kerning = previous != 0 ? atlas.GetKerning(previous, code) * glyphScale : 0.0f;
        const renderer::FontGlyph* glyph = atlas.GetGlyph(code);
        const float advance = (glyph ? glyph->advance : atlas.GetFallbackAdvance()) * glyphScale
                            + text.letterSpacing;
        const float next = lineW + kerning + advance;

        // 折り返し候補は空白「の直後」。行末の空白まで幅に数えると、
        // 右揃え・中央揃えで見えない余白のぶんだけ行がずれる。
        if (code == U' ' || code == U'\t') {
            hasBreak   = true;
            breakEnd   = charBegin;
            breakNext  = offset;
            breakWidth = lineW;
            breakStyle = style;
        }

        // 折り返し。行頭の 1 文字目は、はみ出しても切らない (切ると無限に進まない)。
        // 行の高さより先に判定する。この文字は次の行へ送られるので、先に lineScale へ
        // 畳むと大きい字が「まだ載っていない行」の高さを押し上げる。
        if (limit > 0.0f && next > limit && charBegin > lineBegin) {
            if (hasBreak && breakEnd > lineBegin) {
                pushLine(breakEnd, breakWidth);
                lineBegin = breakNext;
                offset    = breakNext;   // 空白の次から測り直す
                style     = breakStyle;
            } else {
                // 空白の無い長い連なり (日本語・URL 等) は文字単位で折る。
                // charBegin はタグ消化後の位置なので、style を戻すとタグが 1 回失われる。
                pushLine(charBegin, lineW);
                lineBegin = charBegin;
                offset    = charBegin;
            }
            lineW    = 0.0f;
            previous = 0;
            hasBreak = false;
            continue;
        }

        lineScale = (std::max)(lineScale, style.scale);
        lineW     = next;
        previous  = code;
    }
    pushLine(text.text.size(), lineW);
}

// 行の束の高さ。行送りの倍率はここでだけ掛ける。
float TotalTextHeight(const std::vector<UITextLine>& lines, float lineSpacing)
{
    float total = 0.0f;
    for (const UITextLine& line : lines) total += line.height * lineSpacing;
    return total;
}

// 箱に入らない行を落とす。Ellipsis なら最後に残った行へ印を付ける。
// 戻り値は落とした行があったか。
bool ApplyTextOverflow(const UIText& text, std::vector<UITextLine>& lines)
{
    if (text.maxHeight <= 0.0f || text.overflow == TextOverflow::Overflow) return false;

    float used = 0.0f;
    std::size_t kept = 0;
    for (const UITextLine& line : lines) {
        const float next = used + line.height * text.lineSpacing;
        if (next > text.maxHeight) break;
        used = next;
        ++kept;
    }
    // 1 行も入らないなら 1 行だけは残す。何も出ないより、切れていても読めるほうがよい。
    if (kept == 0) kept = 1;
    if (kept >= lines.size()) return false;

    lines.resize(kept);
    if (text.overflow == TextOverflow::Ellipsis && !lines.empty())
        lines.back().ellipsis = true;
    return true;
}

// autoSize が使う「この大きさで箱に入るか」の判定。
bool TextFitsBox(const UIText& text, const std::vector<UITextLine>& lines)
{
    if (text.maxHeight > 0.0f
        && TotalTextHeight(lines, text.lineSpacing) > text.maxHeight) return false;
    if (text.maxWidth > 0.0f) {
        for (const UITextLine& line : lines)
            if (line.width > text.maxWidth) return false;
    }
    return true;
}

// 箱に入る範囲でいちばん大きい文字サイズを二分探索で選ぶ。
// 送り幅はサイズに比例するのでどの段のアトラスで測っても比率は同じ。
// サイズごとに焼き直すと 1 要素の計測で解像度違いの実体が探索回数ぶん生まれる。
float SolveAutoFontSize(const UIText& text, const renderer::FontAtlas& atlas,
                        std::vector<UITextLine>& scratch)
{
    const float upper = text.autoSizeMax > 0.0f ? text.autoSizeMax : text.fontSize;
    const float lower = (std::min)((std::max)(text.autoSizeMin, 1.0f), upper);

    UIText probe = text;
    // 探索の途中で省略が挟まると「入った」の判定が意味を失う。素の状態で測る。
    probe.overflow = TextOverflow::Overflow;

    LayoutTextLines(probe, atlas, upper, scratch);
    if (TextFitsBox(probe, scratch)) return upper;

    // WHY 回数を切るか: 1 px 未満の差は画面に出ない。文字数の多い段落で
    //     探索が長引くほうが害になる。
    float low = lower, high = upper;
    for (int i = 0; i < 8 && high - low > 0.5f; ++i) {
        const float mid = (low + high) * 0.5f;
        LayoutTextLines(probe, atlas, mid, scratch);
        if (TextFitsBox(probe, scratch)) low = mid;
        else                             high = mid;
    }
    return low;
}

// 実測サイズを返し、autoSize の結果を text へ書き戻す。
math::Vector2 ComputeTextLogicalSize(UIText& text,
                                     UISystemContext& ctx,
                                     renderer::ResourceManager& resources)
{
    const std::string& path = text.fontPath.empty() ? ctx.defaultFontPath : text.fontPath;
    renderer::FontAtlas& atlas =
        GetOrLoadFontAtlas(path, ResolveTextRasterPixelHeight(text.fontSize, ctx),
                           ctx, resources);
    if (!atlas.IsValid()) return {};

    // 動的フォントではこの文字列に必要なグリフをここで焼く。幅を測る前に登録が要る。
    // 静的フォントでは即 return する。
    atlas.PrepareText(text.text, resources);

    text.resolvedFontSize = text.autoSize
        ? SolveAutoFontSize(text, atlas, ctx.textLineScratch)
        : text.fontSize;

    LayoutTextLines(text, atlas, text.resolvedFontSize, ctx.textLineScratch);
    ApplyTextOverflow(text, ctx.textLineScratch);

    float maxW = 0.0f;
    for (const UITextLine& line : ctx.textLineScratch)
        maxW = (std::max)(maxW, line.width);

    // 折り返し幅を指定したら箱の幅もそれにする。「一番長い行」に縮めると、
    // 中央揃え・右揃えの基準が文言によって毎回変わる。
    const float width  = text.maxWidth > 0.0f ? text.maxWidth : maxW;
    const float lines  = TotalTextHeight(ctx.textLineScratch, text.lineSpacing);
    // 箱の高さを指定していれば、中身が短くても箱は縮まない。
    // 縦揃えも省略も「余りがある」ことを前提にしているので、ここで確定させる。
    const float height = text.maxHeight > 0.0f ? text.maxHeight : lines;
    return { width, height };
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
        // transform.scale へも書き戻す (後方互換の写し)。この行がある限り文字の
        // transform.scale はユーザーが設定できない。UISystem 内の読み手は全部
        // UIElementSize() 経由へ移したので外せるが、シーンやスクリプトが読んでいないか
        // 実機で確認できていないので残している。
        go.transform.scale.x = size.x;
        go.transform.scale.y = size.y;
    }
    for (int i = 0; i < go.GetChildCount(); ++i)
        if (GameObject* child = go.GetChild(i))
            UpdateTextSizesRecursive(*child, ctx, resources);
}

void UITextSizeSystem(const std::vector<CanvasEntry>& canvases,
                      UISystemContext& ctx,
                      renderer::ResourceManager& resources,
                      float viewportWidth,
                      float viewportHeight)
{
    for (const CanvasEntry& entry : canvases) {
        // 描画と同じ解像度のアトラスを引く。寸法は段が違っても変わらないが、
        // 計測側が別のアトラスを触ると同じ字を 2 つの実体へ焼くことになる。
        ctx.textPixelScale =
            ResolveCanvasPixelScale(*entry.canvas, viewportWidth, viewportHeight);
        UpdateTextSizesRecursive(*entry.go, ctx, resources);
    }
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
    const float fontSize = EffectiveFontSize(text);
    renderer::FontAtlas& atlas =
        GetOrLoadFontAtlas(text.fontPath, ResolveTextRasterPixelHeight(fontSize, ctx),
                           ctx, resources);
    if (!atlas.IsValid() || atlas.GetLineHeight() <= 0.0f) return;

    // 描画経路からも焼いておく。UITextSizeSystem を通らないテキストでもグリフが要る。
    // 登録済みなら UTF-8 走査だけで戻る。
    atlas.PrepareText(text.text, resources);

    const float baseScale = fontSize / atlas.GetLineHeight();

    // 整列と改行位置の決定に行分割が要るので常に事前計算する。
    // Left だけ別扱いにすると UTF-8 走査が 2 通りになり、そちらの方がズレの温床になる。
    std::vector<UITextLine>& lines = ctx.textLineScratch;
    LayoutTextLines(text, atlas, fontSize, lines);
    ApplyTextOverflow(text, lines);
    if (lines.empty()) return;

    // 実測サイズは UITextSizeSystem が同じフレームの描画前に確定させている。
    // 測るのは ComputeTextLogicalSize だけ。2 箇所で出すと描画位置と当たり判定がずれる。
    const math::Vector2 measured = (text.resolvedSize.x > 0.0f || text.resolvedSize.y > 0.0f)
        ? text.resolvedSize
        : math::Vector2{ 0.0f, lines.front().height };

    // 矩形の左上。ここから先は画像とまったく同じ規則で置かれる。
    const UIRect box = ResolveUIRect(parentSize, position, measured, text.anchoring);

    // 縦揃え。箱に余りがあるときだけ意味を持つ (余りが無ければ 0 になる)。
    const float contentHeight = TotalTextHeight(lines, text.lineSpacing);
    const float slack = (std::max)(0.0f, measured.y - contentHeight);
    const float originY = box.position.y
        + (text.verticalAlign == TextVerticalAlign::Middle ? slack * 0.5f
         : text.verticalAlign == TextVerticalAlign::Bottom ? slack : 0.0f);

    // align は「確保した矩形の中で行をどちらへ寄せるか」だけを決める。
    // position の意味まで変えると、中央揃えにした瞬間に文字が左へ半分ずれる。
    auto lineStartX = [&](float lineWidth) -> float {
        if (text.align == TextAlign::Center) return box.position.x + (measured.x - lineWidth) * 0.5f;
        if (text.align == TextAlign::Right)  return box.position.x + (measured.x - lineWidth);
        return box.position.x;
    };

    // ページごとに頂点を分ける。BMFont のマルチページアトラスではグリフごとに参照
    // テクスチャが変わるので、まとめてから分ければ切り替えが最小回数で済む。
    std::vector<std::vector<UIVertex2D>>& pageVerts = ctx.textPageScratch;
    if (pageVerts.size() < atlas.GetPageCount())
        pageVerts.resize(atlas.GetPageCount());
    if (pageVerts.empty()) return;
    const std::size_t pageCount = atlas.GetPageCount();
    for (std::size_t i = 0; i < pageCount; ++i) pageVerts[i].clear();
    pageVerts[0].reserve(text.text.size() * 6);

    // グリフ 1 つを四角形として積む。マスクのクリップもここで通す。
    // ドロー単位で切ると「はみ出した 1 文字のために行ごと消える」になる。
    // 枠のところで文字が半分だけ見えるのが正しい。
    const auto emitGlyph = [&](const renderer::FontGlyph& g, float x, float y,
                               float x2, float y2, float shear,
                               const math::Vector4& color) {
        const std::size_t page =
            (g.page >= 0 && static_cast<std::size_t>(g.page) < pageVerts.size())
                ? static_cast<std::size_t>(g.page) : 0;
        std::vector<UIVertex2D>& verts = pageVerts[page];

        // 疑似イタリックは下端を固定して上端をずらす。字が浮かないよう軸は足元。
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

    // 省略記号の送り幅。行末をどこで打ち切るかの判定に要る。
    const renderer::FontGlyph* ellipsisGlyph = atlas.GetGlyph(U'…');
    const float ellipsisAdvance = ellipsisGlyph
        ? ellipsisGlyph->advance * baseScale + text.letterSpacing : 0.0f;

    // 描く文字数の上限。-1 は無制限。計測は全文で行う ─ 1 文字ずつ出す間に箱が
    // 伸び縮みすると周りのレイアウトが毎フレーム動く。
    const int visibleLimit = text.visibleCharacters;
    int drawnCharacters = 0;

    // 見た目の状態は行をまたいで続く。行ごとに作り直すと </color> が行頭で切れる。
    UIRichTextState style{};
    std::size_t cursor = 0;
    float penY = originY;

    for (std::size_t li = 0; li < lines.size(); ++li) {
        const UITextLine& line = lines[li];
        float penX = lineStartX(line.width);
        // 省略する行は、記号ぶんの幅を残したところで打ち切る。
        // 基準は箱の右端であって、行の開始位置ではない。
        const float lineLimitX = box.position.x + measured.x - ellipsisAdvance;

        char32_t previous = 0;
        cursor = line.begin;
        bool truncatedHere = false;

        while (cursor < line.end) {
            std::size_t charBegin = cursor;
            const char32_t code =
                NextRichGlyph(text.text, cursor, text.richText, fontSize, style, charBegin);
            if (code == 0 || code == U'\n') break;
            // 行末がタグで終わっていると、走査はタグを食べたあと次の行の 1 文字目まで
            // 読み進んでしまう。見た目の変化は残したまま、字だけをここで止める。
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

            // 空白文字のように画像を持たないグリフは、送りだけ進めて四角形を出さない。
            if (g->width > 0.0f && g->height > 0.0f) {
                // 行の下端を揃える。<size> で大きい字が混ざっても足元が動かない。
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
        // t0 はフォントアトラスで UISystem がページごとに差し替える。albedo で潰すと
        // 文字が消えるので、追加の絵は t1 以降 (tex5-tex7) を使うこと。
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

        // kTextVBVertices を超えるテキストはチャンク分割して複数ドローで描く。
        // 3 の倍数へ丸めるのは、4096 が 3 で割り切れず、そのまま切ると境目に
        // 三角形 1 枚に足りない頂点が残って画面を横切る破片になるため。
        // クリップで 1 文字あたりの頂点数が一定でなくなった今は短い文章でも当たりうる。
        constexpr uint32_t kTextChunkVertices = (kTextVBVertices / 3) * 3;
        uint32_t offset = 0;
        const uint32_t total = static_cast<uint32_t>(verts.size());
        while (offset < total) {
            const uint32_t chunk = (std::min)(total - offset, kTextChunkVertices);
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
// containerSize は go 自身の矩形。引数で受けるのは、Canvas が自分の矩形を持たず
// UIElementSize(go) だと 1x1 の箱としてはみ出し警告を出し続けるため。
// 揃えの余りをどれだけずらすかへ直す。
float LayoutAlignOffset(UILayoutAlign align, float slack)
{
    if (slack <= 0.0f) return 0.0f;
    if (align == UILayoutAlign::Center) return slack * 0.5f;
    if (align == UILayoutAlign::End)    return slack;
    return 0.0f;
}

// レイアウトが子のサイズを書き換えてよいか。文字を外すのは、UIText のサイズは
// 実測が正で毎フレーム書き戻されるため (広げても次のフレームには戻る)。
bool LayoutCanResizeChild(GameObject& child)
{
    return !HasMeasuredUISize(child);
}

void ApplyLayout(GameObject& go, const UILayoutGroup& layout,
                 const math::Vector2& containerSize, float canvasScale,
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
    if (indices.empty()) return;

    // 寸法は先に全部集める。揃え・伸縮・Grid の折り返しは、どれも
    // 「全部の大きさが分かってから」でないと解けない。
    std::vector<math::Vector2>& sizes = ctx.layoutSizeScratch;
    sizes.clear();
    sizes.reserve(indices.size());
    for (int idx : indices) {
        // 送り幅は transform.scale ではなく要素サイズから取る。文字の scale は
        // 実測値の写しなので、書き戻しを外した瞬間にレイアウトだけ崩れる。
        sizes.push_back(UIElementSize(*go.GetChild(idx)));
    }

    const float innerW = containerSize.x - layout.paddingLeft - layout.paddingRight;
    const float innerH = containerSize.y - layout.paddingTop  - layout.paddingBottom;

    // ── Grid ──────────────────────────────────────────────────────────────────
    if (layout.axis == UILayoutAxis::Grid) {
        // マス目の大きさ。0 の軸は「その列で最も大きい子」ではなく最初の子に
        // 合わせる ─ マス目が子ごとに変わると格子に見えなくなるため。
        math::Vector2 cell = layout.cellSize;
        if (cell.x <= 0.0f || cell.y <= 0.0f) {
            math::Vector2 largest{};
            for (const math::Vector2& size : sizes) {
                largest.x = (std::max)(largest.x, size.x);
                largest.y = (std::max)(largest.y, size.y);
            }
            if (cell.x <= 0.0f) cell.x = largest.x;
            if (cell.y <= 0.0f) cell.y = largest.y;
        }

        int columns = layout.gridColumns;
        if (columns <= 0) {
            const float stride = cell.x + layout.spacing;
            columns = stride > 0.0f
                ? static_cast<int>((innerW + layout.spacing) / stride) : 1;
        }
        columns = (std::max)(1, columns);

        const int rows = (static_cast<int>(indices.size()) + columns - 1) / columns;
        const float usedW = static_cast<float>(columns) * cell.x
                          + static_cast<float>(columns - 1) * layout.spacing;
        const float usedH = static_cast<float>(rows) * cell.y
                          + static_cast<float>(rows - 1) * layout.spacingCross;
        const float offsetX = LayoutAlignOffset(layout.alignMain,  innerW - usedW);
        const float offsetY = LayoutAlignOffset(layout.alignCross, innerH - usedH);

        for (std::size_t i = 0; i < indices.size(); ++i) {
            GameObject* child = go.GetChild(indices[i]);
            const int column = static_cast<int>(i) % columns;
            const int row    = static_cast<int>(i) / columns;
            const math::Vector2 cellOrigin = {
                layout.paddingLeft + offsetX + static_cast<float>(column) * (cell.x + layout.spacing),
                layout.paddingTop  + offsetY + static_cast<float>(row)    * (cell.y + layout.spacingCross)
            };
            if (layout.expandChildren && LayoutCanResizeChild(*child)) {
                child->transform.scale.x = cell.x;
                child->transform.scale.y = cell.y;
                sizes[i] = cell;
            }
            // マスの中での寄せ。マスと子が同じ大きさなら 0 になる。
            child->transform.position.x =
                cellOrigin.x + LayoutAlignOffset(layout.alignCross, cell.x - sizes[i].x);
            child->transform.position.y =
                cellOrigin.y + LayoutAlignOffset(layout.alignCross, cell.y - sizes[i].y);
        }
        return;
    }

    // ── Horizontal / Vertical ─────────────────────────────────────────────────
    const bool horizontal = layout.axis == UILayoutAxis::Horizontal;
    const float mainInner  = horizontal ? innerW : innerH;
    const float crossInner = horizontal ? innerH : innerW;
    const float totalSpacing = layout.spacing * static_cast<float>(indices.size() - 1);

    float mainUsed = totalSpacing;
    for (const math::Vector2& size : sizes) mainUsed += horizontal ? size.x : size.y;

    // 余りを等分して配る。縮める方向へは配らない (縮めると文字が消えるので、
    // 溢れているという事実は警告で伝えるほうが直せる)。
    float share = 0.0f;
    if (layout.expandChildren) {
        int resizable = 0;
        for (std::size_t i = 0; i < indices.size(); ++i)
            if (LayoutCanResizeChild(*go.GetChild(indices[i]))) ++resizable;
        if (resizable > 0 && mainInner > mainUsed)
            share = (mainInner - mainUsed) / static_cast<float>(resizable);
    }

    // 余りを子へ配りきったなら、束としての余りは残らない = 揃えは効かない。
    const float mainSlack = share > 0.0f ? 0.0f : mainInner - mainUsed;
    const float cursorStart = (horizontal ? layout.paddingLeft : layout.paddingTop)
                            + LayoutAlignOffset(layout.alignMain, mainSlack);
    float cursor = cursorStart;

    for (std::size_t i = 0; i < indices.size(); ++i) {
        GameObject* child = go.GetChild(indices[i]);
        auto& t = child->transform;
        const bool resizable = LayoutCanResizeChild(*child);

        if (share > 0.0f && resizable) {
            if (horizontal) t.scale.x = sizes[i].x + share;
            else            t.scale.y = sizes[i].y + share;
            (horizontal ? sizes[i].x : sizes[i].y) += share;
        }
        if (layout.stretchCross && resizable && crossInner > 0.0f) {
            if (horizontal) { t.scale.y = crossInner; sizes[i].y = crossInner; }
            else            { t.scale.x = crossInner; sizes[i].x = crossInner; }
        }

        const float crossSlack = crossInner - (horizontal ? sizes[i].y : sizes[i].x);
        const float crossPos = (horizontal ? layout.paddingTop : layout.paddingLeft)
                             + LayoutAlignOffset(layout.alignCross, crossSlack);
        if (horizontal) {
            t.position.x = cursor;
            t.position.y = crossPos;
            cursor += sizes[i].x + layout.spacing;
        } else {
            t.position.x = crossPos;
            t.position.y = cursor;
            cursor += sizes[i].y + layout.spacing;
        }
    }

    // paddingRight / paddingBottom がコンテナ末端として機能しているか検証する。
    // クリップが入った今でも、はみ出した中身は「切れて見えない」形で表に出る。
    {
        const float endPos  = cursor - layout.spacing;
        const bool overflow = horizontal
            ? (endPos > containerSize.x - layout.paddingRight)
            : (endPos > containerSize.y - layout.paddingBottom);

        if (overflow && s_warnedLayouts.find(&layout) == s_warnedLayouts.end()) {
            s_warnedLayouts.insert(&layout);
            const float excess = horizontal
                ? (endPos - (containerSize.x - layout.paddingRight))
                : (endPos - (containerSize.y - layout.paddingBottom));
            FBZZ_LOG_WARN("UILayoutGroup: children overflow container by %.1f canvas-px (%.1f viewport-px, canvasScale=%.2f)",
                          excess, excess * canvasScale, canvasScale);
        }
    }
}

// 子の外接矩形 (この要素のローカル座標) を測る。
// 子のアンカーは親のサイズ ─ いま決めようとしている値 ─ に依存するので、
// 循環を避けて「ローカル位置に素直に置かれている」ものとして測る。
math::Vector2 MeasureChildrenExtent(GameObject& go)
{
    math::Vector2 extent{};
    for (int i = 0; i < go.GetChildCount(); ++i) {
        GameObject* child = go.GetChild(i);
        if (!child || !child->activeInHierarchy()) continue;
        const math::Vector2 size = UIElementSize(*child);
        extent.x = (std::max)(extent.x, child->transform.position.x + size.x);
        extent.y = (std::max)(extent.y, child->transform.position.y + size.y);
    }
    return extent;
}

void ApplyContentSizeFitter(GameObject& go, const UIContentSizeFitter& fitter)
{
    const math::Vector2 extent = MeasureChildrenExtent(go);
    auto& scale = go.transform.scale;

    const auto fit = [](UISizeFitMode mode, float measured, float low, float high,
                        float current) {
        if (mode == UISizeFitMode::None) return current;
        float value = measured;
        if (low  > 0.0f) value = (std::max)(value, low);
        if (high > 0.0f) value = (std::min)(value, high);
        return value;
    };

    scale.x = fit(fitter.horizontalFit, extent.x + fitter.padding.x + fitter.padding.z,
                  fitter.minSize.x, fitter.maxSize.x, scale.x);
    scale.y = fit(fitter.verticalFit, extent.y + fitter.padding.y + fitter.padding.w,
                  fitter.minSize.y, fitter.maxSize.y, scale.y);
}

void ApplyUILayoutRecursive(GameObject& go, const math::Vector2& containerSize,
                            float canvasScale, UISystemContext& ctx)
{
    if (!go.activeInHierarchy()) return;

    // 並べるのは降りる前。子のローカル位置が決まっていないと、
    // その子がさらに持つレイアウトの容器サイズも決まらない。
    if (auto* layout = go.GetComponent<UILayoutGroup>(); layout && layout->enabled)
        ApplyLayout(go, *layout, containerSize, canvasScale, ctx);

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ApplyUILayoutRecursive(*child, UIElementSize(*child), canvasScale, ctx);
    }

    // 縮むのは帰りがけ。中身が全部片付いてからでないと外接矩形が出ない。
    if (auto* fitter = go.GetComponent<UIContentSizeFitter>(); fitter && fitter->enabled)
        ApplyContentSizeFitter(go, *fitter);
}

// ── ドラッグ & ドロップの持ち越し ────────────────────────────────────────────
// 落とし先は「離した瞬間にポインターの下にある最前面の受け皿」で、分かるのは階層を
// 全部見終わったあと。走査の途中で拾って後で突き合わせる。
// ファイルスコープなのは、入力を解決するのが GameViewport の 1 パスだけだから
// (Context へ持たせると Viewport ごとに別々の「ドラッグ中」が生まれる)。
struct UIDragFrameState {
    GameObject* hoveredTarget = nullptr;  ///< ポインター下の最前面の受け皿
    GameObject* releasedSource = nullptr; ///< このフレームで離されたドラッグ元
    GameObject* activeSource = nullptr;   ///< ドラッグ中のドラッグ元
};
UIDragFrameState g_dragFrame;

// 直近の入力パスでポインターが UI に吸われたか。
bool     g_pointerOverUI = false;
uint64_t g_pointerOverUIFrame = ~uint64_t{ 0 };

// ── UIButton イベント処理 ─────────────────────────────────────────────────────
// @param groupInteractable 上位の UICanvasGroup が入力を許しているか。
// @param groupBlocksRaycasts false の群は、当たっても「吸わない」(背後へ通す)。
bool ProcessUIEventsRecursive(GameObject& go,
                              UISystemContext& ctx,
                              const UITransform2D& parentTransform,
                              math::Vector2 mouseInCanvasSpace,
                              bool mousePressed,
                              bool inputAvailable = true,
                              bool applySelfTransform = true,
                              std::size_t depth = 0,
                              bool groupInteractable = true,
                              bool groupBlocksRaycasts = true)
{
    if (!go.activeInHierarchy()) return false;

    if (const auto* group = go.GetComponent<UICanvasGroup>(); group && group->enabled) {
        if (group->ignoreParentGroups) {
            groupInteractable   = group->interactable;
            groupBlocksRaycasts = group->blocksRaycasts;
        } else {
            groupInteractable   = groupInteractable   && group->interactable;
            groupBlocksRaycasts = groupBlocksRaycasts && group->blocksRaycasts;
        }
    }
    // 触れない群は配下ごと入力の対象から外す。走査自体は続ける ─ ドラッグ中の
    // 位置追従など、入力を受けない要素にも毎フレーム更新したい状態がある。
    if (!groupInteractable) inputAvailable = false;

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;

    const Rect selfRect = RectFromTransform(go, resolved);
    const bool insideSelf = mouseInCanvasSpace.x >= selfRect.pos.x
        && mouseInCanvasSpace.x <= selfRect.pos.x + selfRect.size.x
        && mouseInCanvasSpace.y >= selfRect.pos.y
        && mouseInCanvasSpace.y <= selfRect.pos.y + selfRect.size.y;
    bool childrenInputAvailable = inputAvailable;
    // Mask も Scroll View も、枠の外に出た中身は触れない。描画のクリップと同じ条件に
    // しないと、スクロールした先の項目を「見えないまま」踏むことになる。
    if (ElementClipsChildren(go) && !insideSelf)
        childrenInputAvailable = false;
    UITransform2D childTransform = resolved;
    // 子のアンカーは「この要素の矩形」に対する割合。Canvas 自身は矩形を持たないので、
    // 呼び出し元が入れた寸法をそのまま渡す。上書きすると transform.scale (既定 1,1) が
    // 親サイズになり、直下の子のアンカーが常に潰れる。
    if (applySelfTransform)
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
                                             true, depth + 1,
                                             groupInteractable, groupBlocksRaycasts);
    }

    auto* button = go.GetComponent<UIButton>();
    if (button) {
        // UIImage の有無に関わらず、解決した矩形に面積があればヒット判定する
        // (UIText や子要素だけで構成されるボタンのため)。
        // 判定に使う矩形は selfRect。scale で決めると「見えているのに押せない」が出る。
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

    // ── 落とし先 ──────────────────────────────────────────────────────────────
    // 走査は最前面から降りてくるので、最初に当たった受け皿がいちばん手前。
    if (auto* target = go.GetComponent<UIDropTarget>()) {
        target->received = false;
        target->hovered  = false;
        if (target->enabled && hit && g_dragFrame.hoveredTarget == nullptr
            && &go != g_dragFrame.activeSource) {
            g_dragFrame.hoveredTarget = &go;
        }
    }

    // ── 掴んで運ぶ ────────────────────────────────────────────────────────────
    if (auto* drag = go.GetComponent<UIDragSource>()) {
        drag->dropped = false;
        drag->droppedOn = {};
        const bool justPressed  = mousePressed && !drag->runtimeLastMouse;
        const bool justReleased = !mousePressed && drag->runtimeLastMouse;

        if (drag->enabled && canReceive && justPressed && hit) {
            drag->dragging = true;
            drag->originPosition = { go.transform.position.x, go.transform.position.y };
            // 掴んだ点と要素の左上のずれ。これを保たないと、掴んだ瞬間に
            // 要素がポインターへ吸い付いて飛ぶ。
            drag->grabOffset = { mouseInCanvasSpace.x - rect.pos.x,
                                 mouseInCanvasSpace.y - rect.pos.y };
            consumed = true;
        }

        if (drag->dragging) {
            g_dragFrame.activeSource = &go;
            if (drag->moveWithPointer) {
                // rect は親の矩形とアンカーを解いた結果なので、その差分だけ動かす。
                // ローカル位置へ直接代入すると、アンカーを付けた要素が飛ぶ。
                const math::Vector2 desired = { mouseInCanvasSpace.x - drag->grabOffset.x,
                                                mouseInCanvasSpace.y - drag->grabOffset.y };
                go.transform.position.x += desired.x - rect.pos.x;
                go.transform.position.y += desired.y - rect.pos.y;
            }
            consumed = true;
            if (justReleased) {
                drag->dragging = false;
                // 落とし先の確定は階層を見終わってから (受け皿がまだ来ていない可能性)。
                g_dragFrame.releasedSource = &go;
            }
        }
        drag->runtimeLastMouse = mousePressed;
    }

    // 素通しの群は「当たったが吸わない」。背後の要素へ判定を渡す。
    if (!groupBlocksRaycasts) return false;
    return consumed;
}

// ドラッグの後始末。走査が終わって初めて「どこへ落ちたか」が確定する。
void ResolveUIDrop()
{
    GameObject* source = g_dragFrame.releasedSource;
    if (!source) return;
    auto* drag = source->GetComponent<UIDragSource>();
    if (!drag) return;

    GameObject* target = g_dragFrame.hoveredTarget;
    UIDropTarget* drop = target ? target->GetComponent<UIDropTarget>() : nullptr;
    // 種類が合わない受け皿は無かったことにする。accepts が空なら何でも受ける。
    if (drop && !drop->accepts.empty() && drop->accepts != drag->payload) {
        drop   = nullptr;
        target = nullptr;
    }

    drag->dropped = true;
    if (drop) {
        drag->droppedOn        = { target->GetID() };
        drop->received         = true;
        drop->receivedFrom     = { source->GetID() };
        drop->receivedPayload  = drag->payload;
    } else if (drag->returnOnDrop) {
        source->transform.position.x = drag->originPosition.x;
        source->transform.position.y = drag->originPosition.y;
    }

}

// UIButton の状態別スプライトを、同じ GameObject の UIImage へ反映する。
// 差し替えは「今どの状態か」の関数でしかないので描画側で当てる。イベント側で書くと、
// 状態が変わらなかったフレームや Play していない Editor で絵が付かない。
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
// テクスチャは非同期に読まれるので要求したフレームには寸法が無く、UV も Border も
// 出せない。1 回きりの解決にするとその状態が固定されて絵が伸びたままになる。
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
    // 同じパスでも、解決済みだったテクスチャがキャッシュから外れていたら取り直す。
    // AssetBrowser の削除はキャッシュを空にするので、パス一致だけで打ち切ると
    // 消えたテクスチャのハンドルを握ったままになる。
    // 解決できなかった参照 (無効ハンドル) は待っても変わらないので再試行しない。
    if (source == image.loadedTexturePath &&
        (!image.texture.IsValid() || resources.Get(image.texture) != nullptr))
        return;

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
// 描くのは RectFromTransform が返した矩形そのもの。別に計算した図だと、ずれたときに
// 「表示のバグ」なのか「判定のバグ」なのか分からない。
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
// 選択中の要素の矩形を、選択マスクへ白で塗る。
// sortOrder を無視するのは、マスクが被覆だけを表すので重なりの前後が結果に出ないため。
// 絵ではなく矩形を塗るのは、アルファでシルエットを取ると暗い絵や絵を持たない
// Mask / Scroll View がマスクに出ないため。
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

    // Canvas 自身 (applySelfTransform == false) は塗らない。選ぶと画面全体が
    // 囲われるだけで、どの要素を見ているのか分からなくなる。
    if (applySelfTransform && rect.size.x > 0.0f && rect.size.y > 0.0f && isSelected(go)) {
        SubmitImage(renderer, resources, ctx, canvasToClip, ctx.selectionMaskPso, layer,
                    rect.pos, rect.size, { 1.0f, 1.0f, 1.0f, 1.0f },
                    { 0.0f, 0.0f }, { 1.0f, 1.0f }, ctx.whiteTexture, resolved.rotationZ);
    }

    UITransform2D childTransform = resolved;
    // Canvas ルートでは上書きしない (ProcessUIEventsRecursive と同じ理由)。
    if (applySelfTransform)
        childTransform.parentSize = rect.size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= Rotate2D(scroll->scrollPosition, resolved.rotationZ);

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            SubmitSelectionMaskRecursive(*child, childTransform, renderer, resources, ctx,
                                         canvasToClip, layer, isSelected, true);
    }
}

// @param groupAlpha 上位の UICanvasGroup を掛け合わせた透明度。
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
                           float groupAlpha = 1.0f)
{
    if (!go.activeInHierarchy()) return;

    // 群の透明度は掛け合わせる。入れ子のフェードが互いを打ち消さないため。
    if (const auto* group = go.GetComponent<UICanvasGroup>(); group && group->enabled) {
        groupAlpha = group->ignoreParentGroups
            ? std::clamp(group->alpha, 0.0f, 1.0f)
            : groupAlpha * std::clamp(group->alpha, 0.0f, 1.0f);
    }
    // 完全に透明なら以下は 1 枚も出ない。子孫ごと降りずに済ませる。
    if (groupAlpha <= 0.0f) return;

    UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;
    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    auto* text   = go.GetComponent<UIText>();

    // 押下中はボタン全体を右下へ沈ませる。pressedColor だけだと暗い背景との乗算後に
    // 差が小さく、入力が UIButton まで届いたのかを画面で判別できない。
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
        // 群の透明度はここで乗せる。コンポーネントの値は書き換えない ─
        // 書き換えるとフェードの途中で保存したときに薄い色が焼き付く。
        color.w *= groupAlpha;

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
        if (groupAlpha >= 1.0f) {
            SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, *text,
                       resolved.position, resolved.parentSize);
        } else {
            // 群の透明度は「今このフレームどう見えるか」でしかない。color へ直接書くと、
            // フェード中に Play を止めた値がシーンの差分として残る。
            UIText faded = *text;
            faded.color.w *= groupAlpha;
            SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, faded,
                       resolved.position, resolved.parentSize);
        }
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
            // アンカーは親の矩形も一緒に見えていないと意味を持たないので実行時は描かない。
            // 実行時に知りたいのは「判定に使われている矩形はどこか」だけ。
            SubmitDebugMarker(renderer, resources, ctx, canvasToClip, pso, layer,
                              { debugRect.pos.x + debugRect.size.x * anchoring.pivot.x,
                                debugRect.pos.y + debugRect.size.y * anchoring.pivot.y },
                              { 1.0f, 0.47f, 0.78f, 0.9f }, 3.0f);
        }
    }

    UITransform2D childTransform = resolved;
    // 子のアンカーは「この要素の矩形」に対する割合になる。
    // Canvas ルートでは上書きしない (ProcessUIEventsRecursive と同じ理由)。
    if (applySelfTransform)
        childTransform.parentSize = RectFromTransform(go, resolved).size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= Rotate2D(scroll->scrollPosition, resolved.rotationZ);

    // Mask / Scroll View はここから下の描画を自分の矩形で切る。
    // 自分自身は切らない (枠の絵を切ると縁が半分消える)。切るのは子孫だけ。
    std::size_t pushedPlanes = 0;
    if (applySelfTransform && ElementClipsChildren(go)) {
        const Rect clipRect = RectFromTransform(go, resolved);
        if (clipRect.size.x > 0.0f && clipRect.size.y > 0.0f)
            pushedPlanes = PushClipRect(ctx.clipPlanes, clipRect, resolved.rotationZ);
    }

    // WHY 参照で受けるか: この並びは子の再帰処理が終わるまで読み続けるので、
    //     深さごとの領域を借りたまま降りる。コピーすると確保が戻ってくる。
    std::vector<GameObject*>& children = AcquireChildScratch(ctx, depth);
    SortUIChildren(go, children);
    for (std::size_t i = 0; i < children.size(); ++i)
        RenderCanvasRecursive(*children[i], childTransform, renderer, resources, ctx,
                              canvasToClip, pso, layer, true, depth + 1, groupAlpha);

    ctx.clipPlanes.resize(ctx.clipPlanes.size() - pushedPlanes);
}

// ── サブシステム ──────────────────────────────────────────────────────────────
void UILayoutSystem(const std::vector<CanvasEntry>& canvases, UISystemContext& ctx,
                    float viewportWidth, float viewportHeight)
{
    for (const CanvasEntry& entry : canvases) {
        const float scale = ResolveCanvasScale(*entry.canvas, viewportWidth, viewportHeight);
        // 並べる基準はセーフエリアの内側。Canvas 直付けの Layout Group が
        // 余白を無視して端まで詰めると、避けたはずの領域へ戻ってしまう。
        ApplyUILayoutRecursive(
            *entry.go,
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight).parentSize,
            scale, ctx);
    }
}

// ── フォーカス移動 ────────────────────────────────────────────────────────────
// 移動先の候補 1 件。矩形は Canvas 空間。
struct UINavCandidate {
    GameObject*   go = nullptr;
    UINavigation* nav = nullptr;
    Rect          rect{};
};

// Canvas 配下から、フォーカスを置ける要素を矩形つきで集める。
// 矩形は入力処理と同じ式で出す。別に計算すると、アンカーやレイアウトが絡んだ画面で
// だけ「隣に見えるのに飛ばない」が出る。
void CollectNavCandidates(GameObject& go, const UITransform2D& parentTransform,
                          bool applySelfTransform, bool groupInteractable,
                          std::vector<UINavCandidate>& out)
{
    if (!go.activeInHierarchy()) return;

    if (const auto* group = go.GetComponent<UICanvasGroup>(); group && group->enabled) {
        groupInteractable = group->ignoreParentGroups
            ? group->interactable
            : (groupInteractable && group->interactable);
    }

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform, parentTransform.parentSize)
        : parentTransform;
    const Rect selfRect = RectFromTransform(go, resolved);

    if (groupInteractable) {
        if (auto* nav = go.GetComponent<UINavigation>();
            nav && nav->enabled && selfRect.size.x > 0.0f && selfRect.size.y > 0.0f) {
            out.push_back({ &go, nav, selfRect });
        }
    }

    UITransform2D childTransform = resolved;
    if (applySelfTransform) childTransform.parentSize = selfRect.size;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= Rotate2D(scroll->scrollPosition, resolved.rotationZ);

    for (int i = 0; i < go.GetChildCount(); ++i)
        if (GameObject* child = go.GetChild(i))
            CollectNavCandidates(*child, childTransform, true, groupInteractable, out);
}

// 方向 dir (単位ベクトル) にいちばん近い候補。無ければ nullptr。
// 距離だけで選ぶと斜め後ろの近い項目が勝ち、下キーで上へ戻ることが起きる。
// 進行方向の成分が横ずれを上回るものだけを候補にする。
const UINavCandidate* FindNearestInDirection(const std::vector<UINavCandidate>& candidates,
                                             const Rect& from, const math::Vector2& dir,
                                             const GameObject* exclude)
{
    const math::Vector2 origin = { from.pos.x + from.size.x * 0.5f,
                                   from.pos.y + from.size.y * 0.5f };
    const UINavCandidate* best = nullptr;
    float bestScore = 0.0f;

    for (const UINavCandidate& candidate : candidates) {
        if (candidate.go == exclude) continue;
        const math::Vector2 center = { candidate.rect.pos.x + candidate.rect.size.x * 0.5f,
                                       candidate.rect.pos.y + candidate.rect.size.y * 0.5f };
        const math::Vector2 delta = { center.x - origin.x, center.y - origin.y };
        const float along  = delta.x * dir.x + delta.y * dir.y;
        if (along <= 0.0f) continue;                    // 進みたい向きの逆
        const float lateral = std::fabs(delta.x * dir.y - delta.y * dir.x);
        if (lateral > along) continue;                  // 45 度より外は「その方向」ではない

        // 横ずれを重く見る。同じ距離なら真っ直ぐ並んでいるほうを選ぶ。
        const float score = along + lateral * 2.0f;
        if (!best || score < bestScore) {
            best = &candidate;
            bestScore = score;
        }
    }
    return best;
}

// 方向入力とその繰り返し。操作しているのは 1 人でフォーカスも画面全体で 1 つなので、
// ファイルスコープに 1 組だけ置く (Canvas ごとだと重なったとき速度がばらつく)。
math::Vector2 g_navHeldDirection{};
float         g_navRepeatTimer = 0.0f;

// いま倒されている方向。倒していなければゼロ。
math::Vector2 ReadNavigationDirection()
{
    math::Vector2 dir{};
    if (input::Input::KeyHeld(input::KeyCode::LEFT))  dir.x -= 1.0f;
    if (input::Input::KeyHeld(input::KeyCode::RIGHT)) dir.x += 1.0f;
    if (input::Input::KeyHeld(input::KeyCode::UP))    dir.y -= 1.0f;
    if (input::Input::KeyHeld(input::KeyCode::DOWN))  dir.y += 1.0f;

    const int pad = input::Gamepad::GetFirstConnectedPad();
    if (pad >= 0) {
        if (input::Gamepad::ButtonHeld(input::GamepadButton::DPAD_LEFT,  pad)) dir.x -= 1.0f;
        if (input::Gamepad::ButtonHeld(input::GamepadButton::DPAD_RIGHT, pad)) dir.x += 1.0f;
        if (input::Gamepad::ButtonHeld(input::GamepadButton::DPAD_UP,    pad)) dir.y -= 1.0f;
        if (input::Gamepad::ButtonHeld(input::GamepadButton::DPAD_DOWN,  pad)) dir.y += 1.0f;
        // スティックは倒し込みで初めて 1 方向として扱う。
        // 生値は静止時も 0 にならないので、デッドゾーンが無いとフォーカスが流れ続ける。
        constexpr float kStickThreshold = 0.6f;
        const float sx = input::Gamepad::Axis(input::GamepadAxis::LEFT_STICK_X, pad);
        const float sy = input::Gamepad::Axis(input::GamepadAxis::LEFT_STICK_Y, pad);
        if (sx < -kStickThreshold) dir.x -= 1.0f;
        if (sx >  kStickThreshold) dir.x += 1.0f;
        if (sy >  kStickThreshold) dir.y -= 1.0f;   // スティックの +Y は上、Canvas の +Y は下
        if (sy < -kStickThreshold) dir.y += 1.0f;
    }

    // 斜めは扱わない。UI は縦横の格子で並んでいるので、斜めを許すと
    // 「どちらへ行きたかったのか」が入力から決まらない。
    if (std::fabs(dir.x) > std::fabs(dir.y)) return { dir.x > 0.0f ? 1.0f : -1.0f, 0.0f };
    if (std::fabs(dir.y) > 0.0f)             return { 0.0f, dir.y > 0.0f ? 1.0f : -1.0f };
    return {};
}

bool ReadNavigationSubmit()
{
    if (input::Input::KeyHeld(input::KeyCode::ENTER)) return true;
    if (input::Input::KeyHeld(input::KeyCode::SPACE)) return true;
    const int pad = input::Gamepad::GetFirstConnectedPad();
    return pad >= 0 && input::Gamepad::ButtonHeld(input::GamepadButton::A, pad);
}

// フォーカスを更新し、決まった位置へポインターを移す。
// 動かしたら true (呼び出し側はこのフレームのポインターをフォーカスへ寄せる)。
bool UINavigationSystem(Scene& scene, UICanvas& canvas, GameObject& canvasGO,
                        const UITransform2D& canvasRoot,
                        std::vector<UINavCandidate>& scratch)
{
    scratch.clear();
    CollectNavCandidates(canvasGO, canvasRoot, false, true, scratch);
    for (UINavCandidate& candidate : scratch) candidate.nav->focused = false;
    if (scratch.empty()) return false;

    // いまのフォーカス。消えていたら選び直す。
    const UINavCandidate* current = nullptr;
    for (const UINavCandidate& candidate : scratch)
        if (candidate.go->GetID() == canvas.focusedObject.id) { current = &candidate; break; }

    if (!current) {
        for (const UINavCandidate& candidate : scratch)
            if (candidate.nav->selectOnStart) { current = &candidate; break; }
        if (!current) current = &scratch.front();
        canvas.focusedObject = { current->go->GetID() };
    }

    // 方向入力の繰り返し。倒し始めは即座に、以降は間隔をあけて 1 段ずつ。
    const math::Vector2 dir = ReadNavigationDirection();
    bool step = false;
    if (dir.x == 0.0f && dir.y == 0.0f) {
        g_navHeldDirection = {};
        g_navRepeatTimer   = 0.0f;
    } else if (dir.x != g_navHeldDirection.x || dir.y != g_navHeldDirection.y) {
        g_navHeldDirection = dir;
        g_navRepeatTimer   = canvas.navigationRepeatDelay;
        step = true;
    } else {
        // WHY 未スケールの時間か: ポーズ中のメニューは timeScale = 0 で動く。
        //     スケール済みの刻みを使うと、そこでだけ方向キーの連続入力が止まる。
        g_navRepeatTimer -= fbzz::Time::unscaledDeltaTime;
        if (g_navRepeatTimer <= 0.0f) {
            g_navRepeatTimer = canvas.navigationRepeatInterval;
            step = true;
        }
    }

    if (step) {
        const UINavigation& nav = *current->nav;
        GameObject* explicitNext = nullptr;
        if (nav.mode == UINavigationMode::Explicit) {
            const EntityRef& link = dir.x < 0.0f ? nav.left
                                  : dir.x > 0.0f ? nav.right
                                  : dir.y < 0.0f ? nav.up
                                                 : nav.down;
            explicitNext = link.Resolve(scene);
        }

        if (nav.mode != UINavigationMode::None) {
            const UINavCandidate* next = nullptr;
            if (explicitNext) {
                for (const UINavCandidate& candidate : scratch)
                    if (candidate.go == explicitNext) { next = &candidate; break; }
            } else if (nav.mode == UINavigationMode::Automatic) {
                next = FindNearestInDirection(scratch, current->rect, dir, current->go);
            }
            if (next) {
                current = next;
                canvas.focusedObject = { next->go->GetID() };
            }
        }
    }

    current->nav->focused = true;

    // ポインターをフォーカスの中心へ置く。ここから先は既存のウィジェットが
    // マウスと同じように反応する (UIPointer.hpp の WHY)。
    UIPointer::Set({ current->rect.pos.x + current->rect.size.x * 0.5f,
                     current->rect.pos.y + current->rect.size.y * 0.5f },
                   ReadNavigationSubmit());
    return true;
}

void UIEventSystem(Scene& scene,
                   const std::vector<CanvasEntry>& canvases,
                   UISystemContext& ctx,
                   float viewportWidth,
                   float viewportHeight,
                   math::Vector2 rawMouseInViewport,
                   bool mousePressed,
                   const math::Matrix4& viewProjection,
                   math::Vector3 cameraWorldPos,
                   UIRenderTargetView targetView)
{
    // ドラッグの持ち越しはフレームごとに作り直す。
    // 前フレームの受け皿が残っていると、離した瞬間に古い相手へ落ちる。
    g_dragFrame = {};

    // ナビゲーションはポインターを差し替えるので、当たり判定より先に解く。
    // 既に差し替えが宣言されていれば手を出さない (ゲーム内カーソルを持つ画面が優先)。
    if (!UIPointer::IsActive()) {
        static std::vector<UINavCandidate> navScratch;
        for (const CanvasEntry& entry : canvases) {
            if (!entry.canvas->navigationEnabled) continue;
            if (!ShouldRenderCanvas(*entry.canvas, targetView)) continue;
            const UITransform2D canvasRoot =
                BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
            if (UINavigationSystem(scene, *entry.canvas, *entry.go, canvasRoot, navScratch))
                break;   // フォーカスは画面に 1 つ。最初に受け持った Canvas が持つ。
        }
    }

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
            const UITransform2D canvasRoot =
                BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
            const math::Vector2 pointerPx =
                UIPointer::IsActive() ? UIPointer::Position() : canvasPx;
            const bool pointerPressed =
                UIPointer::IsActive() ? UIPointer::Pressed() : mousePressed;
            entry.canvas->resolvedMousePosition = canvasPx;
            inputConsumed |= ProcessUIEventsRecursive(*entry.go, ctx, canvasRoot, pointerPx,
                                                      pointerPressed, !inputConsumed, false);
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

        const UITransform2D canvasRoot =
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
        // ゲーム内カーソルがあればそちらを唯一のポインターとして使う。
        // 判定は「Canvas 空間の座標 1 つと押下状態」だけで決まるので、入口を 1 つに絞れば
        // マウスとパッドの両対応をウィジェット側に書かずに済む (UIPointer.hpp)。
        const math::Vector2 pointer =
            UIPointer::IsActive() ? UIPointer::Position() : mouseInCanvas;
        const bool pointerPressed =
            UIPointer::IsActive() ? UIPointer::Pressed() : mousePressed;

        // ここへ残すのは常に「OS のマウスを Canvas 空間へ直した値」。差し替え後の値を
        // 返すと、カーソルを動かす側が自分の出力を読み直してマウスで動かせなくなる。
        // 式は renderMode / Canvas Scaler / viewport 寸法で決まるので、スクリプト側で
        // 書き直させると viewport ≠ ウィンドウの場面でだけ静かにずれる。
        entry.canvas->resolvedMousePosition = mouseInCanvas;
        inputConsumed |= ProcessUIEventsRecursive(*entry.go, ctx, canvasRoot, pointer,
                                                  pointerPressed, !inputConsumed, false);
    }

    // 落とし先が決まるのは全 Canvas を見終わったあと。
    ResolveUIDrop();

    // ゲーム側が「UI の上か」を判定できるように残す。
    g_pointerOverUI      = inputConsumed;
    g_pointerOverUIFrame = fbzz::Time::frameCount;
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
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
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

bool UIPointerOverUI()
{
    // WHY フレーム番号で照合するか: UI が回っていないフレーム (Play 前・UI の無い
    //     シーン) で前回の値が残っていると、ゲーム入力が理由もなく止まる。
    return g_pointerOverUIFrame == fbzz::Time::frameCount && g_pointerOverUI;
}

std::size_t UITextVisibleLength(const std::string& text, bool richText)
{
    UIRichTextState style{};
    std::size_t count = 0;
    std::size_t offset = 0;
    std::size_t begin  = 0;
    // fontSize は <size> の解釈にしか使わない。文字数には影響しないので 1 でよい。
    while (NextRichGlyph(text, offset, richText, 1.0f, style, begin) != 0)
        ++count;
    return count;
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
    BeginUIFrame(ctx);

    if (!ctx.shader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() || !ctx.whiteTexture.IsValid())
        return;

    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    UITextSizeSystem(canvases, ctx, resources, viewportWidth, viewportHeight);
    UILayoutSystem(canvases, ctx, viewportWidth, viewportHeight);
    // Editor は同じ Scene を 1 フレームに複数回描く。各 Viewport で lastMouseState を
    // 更新すると、後続のパスが GameViewport の生成したクリックを消してしまう。
    if (targetView == UIRenderTargetView::GameViewport) {
        UIEventSystem(scene, canvases, ctx, viewportWidth, viewportHeight, mouseInViewport,
                      mousePressed, viewProjection, cameraWorldPos, targetView);
    }
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

    EnsureInit(ctx, resources);
    BeginUIFrame(ctx);

    if (!ctx.shader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.selectionMaskPso.IsValid() || !ctx.whiteTexture.IsValid())
        return;

    // レイアウトはまだ本描画の前なので、この時点の transform で解く。
    // UILayoutGroup も UIText の実測も前フレームぶんが載っており、1 フレーム古い矩形に
    // なるのは動いている最中だけ。
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
        SubmitSelectionMaskRecursive(*entry.go, canvasRoot, renderer, resources, ctx,
                                     state.canvasToClip, state.layer, isSelected, false);
    }
}

} // namespace fbzz::scene
