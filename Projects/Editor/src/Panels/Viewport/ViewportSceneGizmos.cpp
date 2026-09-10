/// @file    ViewportSceneGizmos.cpp
/// @brief   Scene View のカメラ・ライトアイコンと3D Gizmo。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "ViewportCommon.hpp"
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/ViewportCamera.hpp>
// メッシュを持たないコンポーネントのアイコン描画に必要な型。
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ForceField.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>

namespace fbzz::editor {

namespace {

bool GizmoTransformEquals(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z &&
           lhs.rotation.x == rhs.rotation.x &&
           lhs.rotation.y == rhs.rotation.y &&
           lhs.rotation.z == rhs.rotation.z &&
           lhs.rotation.w == rhs.rotation.w &&
           lhs.scale.x == rhs.scale.x &&
           lhs.scale.y == rhs.scale.y &&
           lhs.scale.z == rhs.scale.z;
}

// worldRow (行優先ワールド行列) をローカル TRS に分解して transform へ書き戻す。
// ImGuizmo decomposes with Euler angles that do not match the engine quaternion convention.
// Extract TRS directly from the matrix to avoid handedness and sign mismatches.
void ApplyWorldRowToTransform(scene::GameObject& go, const math::Matrix4& worldRow)
{
    math::Matrix4 localRow = worldRow;
    if (scene::GameObject* parent = go.GetParent()) {
        const math::Matrix4 parentInv = math::Matrix4::Inverse(parent->transform.GetWorldMatrix());
        localRow = parentInv * worldRow;
    }

    const float sx = std::sqrt(localRow.m[0][0]*localRow.m[0][0] + localRow.m[1][0]*localRow.m[1][0] + localRow.m[2][0]*localRow.m[2][0]);
    const float sy = std::sqrt(localRow.m[0][1]*localRow.m[0][1] + localRow.m[1][1]*localRow.m[1][1] + localRow.m[2][1]*localRow.m[2][1]);
    const float sz = std::sqrt(localRow.m[0][2]*localRow.m[0][2] + localRow.m[1][2]*localRow.m[1][2] + localRow.m[2][2]*localRow.m[2][2]);

    math::Matrix4 rotMat = math::Matrix4::Identity();
    if (!math::NearlyZero(sx)) { rotMat.m[0][0] = localRow.m[0][0]/sx; rotMat.m[1][0] = localRow.m[1][0]/sx; rotMat.m[2][0] = localRow.m[2][0]/sx; }
    if (!math::NearlyZero(sy)) { rotMat.m[0][1] = localRow.m[0][1]/sy; rotMat.m[1][1] = localRow.m[1][1]/sy; rotMat.m[2][1] = localRow.m[2][1]/sy; }
    if (!math::NearlyZero(sz)) { rotMat.m[0][2] = localRow.m[0][2]/sz; rotMat.m[1][2] = localRow.m[1][2]/sz; rotMat.m[2][2] = localRow.m[2][2]/sz; }

    go.transform.position = { localRow.m[0][3], localRow.m[1][3], localRow.m[2][3] };
    go.transform.scale    = { sx, sy, sz };
    go.transform.rotation = math::Quaternion::FromMatrix4(rotMat);
}

} // namespace

bool WorldToScreen(const math::Vector3& world,
                   const EditorContext& ctx,
                   const ImVec2& vpMin, const ImVec2& vpSize,
                   ImVec2& out)
{
    if (!ctx.editorCamera) return false;
    const math::Matrix4 vp = ctx.editorCamera->GetProjectionMatrix()
                           * ctx.editorCamera->GetViewMatrix();
    const math::Vector4 clip = vp * math::Vector4{ world.x, world.y, world.z, 1.0f };
    if (clip.w <= 0.001f) return false;
    const float ndcX =  clip.x / clip.w;
    const float ndcY = -clip.y / clip.w;
    out = {
        vpMin.x + (ndcX * 0.5f + 0.5f) * vpSize.x,
        vpMin.y + (ndcY * 0.5f + 0.5f) * vpSize.y
    };
    return true;
}

void DrawDirectionLine(EditorContext& ctx,
                       ImDrawList* dl,
                       const math::Vector3& start,
                       const math::Vector3& direction,
                       float length,
                       const ImVec2& vpMin,
                       const ImVec2& vpSize,
                       ImU32 color)
{
    ImVec2 a;
    ImVec2 b;
    if (!WorldToScreen(start, ctx, vpMin, vpSize, a)) return;
    if (!WorldToScreen(start + direction.Normalized() * length, ctx, vpMin, vpSize, b)) return;

    dl->AddLine(a, b, color, 2.0f);

    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.001f) return;

    const float ux = dx / len;
    const float uy = dy / len;
    const ImVec2 left  = { b.x - ux * 9.0f - uy * 4.5f, b.y - uy * 9.0f + ux * 4.5f };
    const ImVec2 right = { b.x - ux * 9.0f + uy * 4.5f, b.y - uy * 9.0f - ux * 4.5f };
    dl->AddTriangleFilled(b, left, right, color);
}

void DrawCameraFrustum(EditorContext& ctx,
                       ImDrawList* dl,
                       const scene::Transform& transform,
                       const scene::CameraComponent& camera,
                       const ImVec2& vpMin,
                       const ImVec2& vpSize,
                       ImU32 color)
{
    const float nearDist = 0.45f;
    const float farDist = 2.5f;
    const float aspect = 16.0f / 9.0f;
    const float tanHalfFov = std::tanf(math::ToRad(camera.fovY) * 0.5f);

    const math::Vector3 origin = transform.position;
    const math::Vector3 forward = transform.forward.Normalized();
    const math::Vector3 right = transform.right.Normalized();
    const math::Vector3 up = transform.up.Normalized();

    auto corner = [&](float dist, float sx, float sy) {
        const float halfHeight = tanHalfFov * dist;
        const float halfWidth = halfHeight * aspect;
        return origin + forward * dist + right * (sx * halfWidth) + up * (sy * halfHeight);
    };

    const math::Vector3 nearCorners[4] = {
        corner(nearDist, -1.0f,  1.0f),
        corner(nearDist,  1.0f,  1.0f),
        corner(nearDist,  1.0f, -1.0f),
        corner(nearDist, -1.0f, -1.0f)
    };
    const math::Vector3 farCorners[4] = {
        corner(farDist, -1.0f,  1.0f),
        corner(farDist,  1.0f,  1.0f),
        corner(farDist,  1.0f, -1.0f),
        corner(farDist, -1.0f, -1.0f)
    };

    ImVec2 nearScreen[4];
    ImVec2 farScreen[4];
    bool nearVisible[4];
    bool farVisible[4];
    for (int i = 0; i < 4; ++i) {
        nearVisible[i] = WorldToScreen(nearCorners[i], ctx, vpMin, vpSize, nearScreen[i]);
        farVisible[i] = WorldToScreen(farCorners[i], ctx, vpMin, vpSize, farScreen[i]);
    }

    for (int i = 0; i < 4; ++i) {
        const int next = (i + 1) % 4;
        if (nearVisible[i] && nearVisible[next])
            dl->AddLine(nearScreen[i], nearScreen[next], color, 1.5f);
        if (farVisible[i] && farVisible[next])
            dl->AddLine(farScreen[i], farScreen[next], color, 1.5f);
        if (nearVisible[i] && farVisible[i])
            dl->AddLine(nearScreen[i], farScreen[i], color, 1.5f);
    }
}

namespace {

// コンポーネント型をラムダ引数として渡すためのタグ。
// WHY: ジェネリックラムダはテンプレート引数を直接受け取れず、実体を作らせると
//      非デフォルト構築のコンポーネントで壊れる。型だけを運ぶ空のタグを使う。
template<typename T> struct ComponentTag { using Type = T; };

// アイコン 1 個ぶんの描画コンテキスト。
struct IconDraw {
    ImDrawList* dl        = nullptr;
    ImVec2      screenPos{};
    ImU32       color     = 0;
    bool        selected  = false;
};

// ワールド座標をスクリーンへ投影しつつ、視錐台の内外を判定する。
// WHY: 従来は「カメラ背面か」と「画面矩形内か」しか見ておらず、遠平面より奥のオブジェクトも
//      投影して描いていた。NDC の z も見て、視錐台の外は描画前に切り捨てる。
bool ProjectIcon(const math::Vector3& world,
                 const EditorContext& ctx,
                 const ImVec2& vpMin, const ImVec2& vpSize,
                 ImVec2& out)
{
    if (!ctx.editorCamera) return false;
    const math::Matrix4 vp = ctx.editorCamera->GetProjectionMatrix()
                           * ctx.editorCamera->GetViewMatrix();
    const math::Vector4 clip = vp * math::Vector4{ world.x, world.y, world.z, 1.0f };
    if (clip.w <= 0.001f) return false;

    const float ndcX =  clip.x / clip.w;
    const float ndcY = -clip.y / clip.w;
    const float ndcZ =  clip.z / clip.w;
    // 近平面手前・遠平面より奥は描かない (DirectX の NDC は z ∈ [0,1])。
    if (ndcZ < 0.0f || ndcZ > 1.0f) return false;
    // 画面外は少しだけ余裕を持って切る (アイコンの半径ぶん)。
    constexpr float kMargin = 24.0f;
    const float sx = vpMin.x + (ndcX * 0.5f + 0.5f) * vpSize.x;
    const float sy = vpMin.y + (ndcY * 0.5f + 0.5f) * vpSize.y;
    if (sx < vpMin.x - kMargin || sx > vpMin.x + vpSize.x + kMargin ||
        sy < vpMin.y - kMargin || sy > vpMin.y + vpSize.y + kMargin) return false;

    out = { sx, sy };
    return true;
}

// 汎用バッジアイコン (角丸の四角 + 1 文字)。
// WHY: メッシュを持たないコンポーネントは種類が多く、1 つずつ専用の図形を描くと
//      コード量に見合わない。色と頭文字で識別できれば選択とデバッグには足りる。
void DrawBadgeIcon(const IconDraw& icon, const char* glyph)
{
    constexpr float kHalf = 9.0f;
    const ImVec2 a = { icon.screenPos.x - kHalf, icon.screenPos.y - kHalf };
    const ImVec2 b = { icon.screenPos.x + kHalf, icon.screenPos.y + kHalf };

    icon.dl->AddRectFilled(a, b, icon.color, 4.0f);
    icon.dl->AddRect(a, b, IM_COL32(0, 0, 0, 140), 4.0f, 0, 1.5f);
    if (icon.selected)
        icon.dl->AddRect({ a.x - 3.0f, a.y - 3.0f }, { b.x + 3.0f, b.y + 3.0f },
                         IM_COL32(255, 220, 60, 255), 6.0f, 0, 2.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(glyph);
    icon.dl->AddText({ icon.screenPos.x - textSize.x * 0.5f,
                       icon.screenPos.y - textSize.y * 0.5f },
                     IM_COL32(15, 15, 15, 230), glyph);
}

// 色の不透明度を差し替える (非アクティブ表示用)。
ImU32 WithAlpha(ImU32 color, int alpha)
{
    return (color & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

} // namespace

void DrawSceneIcons(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize)
{
    if (!ctx.activeScene || !ctx.editorCamera) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto isSelected = [&ctx](scene::EntityID id) {
        return std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id)
            != ctx.selectedEntities.end();
    };

    // コンポーネント種別ごとにアイコンを描く共通ループ。
    //
    // WHY: 以前は Scene の全 GameObject を毎フレーム走査し、1 体ずつ投影していた。
    //      VFX Graph は 1 エフェクトにつきノード数ぶんの GameObject を作るため、
    //      アイコンを持たない数千体まで投影計算にかけることになる。
    //      GetEntities<T>() で「そのコンポーネントを持つ実体」だけを引けば走査量が桁で減る。
    auto forEachIcon = [&](auto componentTag, ImU32 baseColor, auto&& draw) {
        using T = typename decltype(componentTag)::Type;
        for (const scene::EntityID id : ctx.activeScene->GetEntities<T>()) {
            scene::GameObject* go = ctx.activeScene->GetGameObject(id);
            if (!go) continue;

            ImVec2 sp;
            if (!ProjectIcon(go->transform.position, ctx, vpMin, vpSize, sp)) continue;

            // WHY: 非アクティブなオブジェクトを完全に隠すと、ビューポートから選び直せなくなる
            //      (Hierarchy でしか触れない)。半透明にして「居るが無効」を表す。
            const bool active = go->activeInHierarchy();
            IconDraw icon;
            icon.dl        = dl;
            icon.screenPos = sp;
            icon.selected  = isSelected(id);
            icon.color     = active ? baseColor : WithAlpha(baseColor, 70);

            draw(icon, *go, active);
        }
    };

    // ── ライト ────────────────────────────────────────────────────────────────
    forEachIcon(ComponentTag<scene::LightComponent>{}, IM_COL32(255, 200, 60, 200),
                [&](const IconDraw& icon, scene::GameObject& go, bool active) {
        auto* light = go.GetComponent<scene::LightComponent>();
        if (!light) return;

        constexpr float kR   = 8.0f;
        constexpr float kRay = 14.0f;
        constexpr int   kRays = 8;
        const ImU32 col = icon.selected ? IM_COL32(255, 220, 60, 255) : icon.color;

        icon.dl->AddCircleFilled(icon.screenPos, kR, col);
        icon.dl->AddCircle(icon.screenPos, kR, IM_COL32(0, 0, 0, 120), 16, 1.5f);
        for (int i = 0; i < kRays; ++i) {
            const float ang = static_cast<float>(i) * (2.0f * 3.14159265f / kRays);
            const ImVec2 a = { icon.screenPos.x + std::cosf(ang) * (kR + 3.0f),
                               icon.screenPos.y + std::sinf(ang) * (kR + 3.0f) };
            const ImVec2 b = { icon.screenPos.x + std::cosf(ang) * (kR + kRay),
                               icon.screenPos.y + std::sinf(ang) * (kR + kRay) };
            icon.dl->AddLine(a, b, col, 1.5f);
        }
        // NOTE: 範囲・形状のワイヤーはここでは描かない。Overlay の "Light Range" に
        //       繋がった LightRangeDebugPass (Engine 側の DebugDraw) が担当する。
        //       両方で描くと二重線になり、チェックボックスも片方にしか効かない。
        if (active && light->enabled && light->type == scene::LightComponent::Type::Directional)
            DrawDirectionLine(ctx, icon.dl, go.transform.position, go.transform.forward,
                              2.5f, vpMin, vpSize, col);
    });

    // ── カメラ ────────────────────────────────────────────────────────────────
    forEachIcon(ComponentTag<scene::CameraComponent>{}, IM_COL32(120, 200, 255, 200),
                [&](const IconDraw& icon, scene::GameObject& go, bool active) {
        auto* camera = go.GetComponent<scene::CameraComponent>();
        if (!camera) return;

        constexpr float kW = 16.0f, kH = 11.0f;
        constexpr float kLW = 7.0f, kLH = 5.0f, kLX = 9.0f;
        const ImU32 col  = icon.selected ? IM_COL32(255, 220, 60, 255) : icon.color;
        const ImU32 dark = IM_COL32(0, 0, 0, 140);
        const ImVec2 sp  = icon.screenPos;

        icon.dl->AddRectFilled({ sp.x - kW, sp.y - kH * 0.5f },
                               { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, col, 2.0f);
        icon.dl->AddRect({ sp.x - kW, sp.y - kH * 0.5f },
                         { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, dark, 2.0f);
        ImVec2 lens[4] = {
            { sp.x + kW * 0.4f, sp.y - kLH },
            { sp.x + kLX,       sp.y - kLW },
            { sp.x + kLX,       sp.y + kLW },
            { sp.x + kW * 0.4f, sp.y + kLH },
        };
        icon.dl->AddConvexPolyFilled(lens, 4, col);
        icon.dl->AddPolyline(lens, 4, dark, ImDrawFlags_Closed, 1.0f);
        DrawDirectionLine(ctx, icon.dl, go.transform.position, go.transform.forward,
                          2.0f, vpMin, vpSize, col);
        if (active && camera->enabled)
            DrawCameraFrustum(ctx, icon.dl, go.transform, *camera, vpMin, vpSize, col);
    });

    // ── メッシュを持たないその他のコンポーネント ──────────────────────────────
    // WHY: これらはビューポート上に一切表示されず、Hierarchy からしか選べなかった。
    //      配置を目で確認できないと、音源やフォースフィールドの位置調整ができない。
    const auto badge = [](const char* glyph) {
        return [glyph](const IconDraw& icon, scene::GameObject&, bool) {
            DrawBadgeIcon(icon, glyph);
        };
    };

    forEachIcon(ComponentTag<scene::AudioSourceComponent>{},      IM_COL32(140, 220, 150, 210), badge("A"));
    forEachIcon(ComponentTag<scene::ParticleEmitter>{},           IM_COL32(230, 150, 230, 210), badge("P"));
    forEachIcon(ComponentTag<scene::ForceField>{},        IM_COL32(200, 120, 240, 210), badge("F"));
    // 環境風は ForceField の 1 種類になったので、専用アイコンは持たない。
    forEachIcon(ComponentTag<scene::NavMeshAgentComponent>{},     IM_COL32(120, 190, 120, 210), badge("N"));
    forEachIcon(ComponentTag<scene::ReflectionProbeComponent>{},  IM_COL32(190, 190, 240, 210), badge("R"));
    forEachIcon(ComponentTag<scene::DecalComponent>{},            IM_COL32(240, 180, 120, 210), badge("D"));
}

// ---------------------------------------------------------------------------
// ナビゲーションギズモ (Blender / Godot 風の軸ボール)
// ---------------------------------------------------------------------------
// WHY: 以前は ImGuizmo::ViewManipulate の立方体を出していたが、
//      「今どの面を向いているか」「クリックするとどこへ回り込むか」が読み取りづらく、
//      軸名も出ないため方向感覚を保てなかった。Blender / Godot と同じく
//      ±X / ±Y / ±Z をボールで直接示し、操作を次の 2 つだけに整理する。
//        ボールをクリック   : その軸から見る視点へスムーズに回り込む
//        ギズモ内をドラッグ : ピボット周回 (オービット)
namespace {

constexpr float kNavSize      = 104.0f;  // ギズモ全体の一辺 (px)
constexpr float kNavPadding   = 10.0f;   // ビューポート右上からの余白
constexpr float kNavBallNearR = 11.0f;   // 手前側のボール半径
constexpr float kNavBallFarR  = 7.5f;    // 奥側のボール半径 (小さくして奥行きを出す)
constexpr float kNavSnapTime  = 0.26f;   // 軸クリック時の視点移動にかける秒数
constexpr float kNavDragSlop  = 4.0f;    // クリックとドラッグを分けるしきい値 (px)
constexpr float kNavOrbitSens = 0.45f;   // ドラッグ 1px あたりの回転角度

// 6 方向の定義。描画・当たり判定はこの並びの添字で参照する。
struct NavAxis {
    math::Vector3 dir;         // ワールド方向
    int           colorIndex;  // kNavAxisColor の添字
    const char*   label;       // ボールに出す軸名
    bool          positive;    // 正方向のみ中心から線を引き、ラベルを常時出す
};

const NavAxis kNavAxes[6] = {
    { {  1.0f,  0.0f,  0.0f }, 0, "X", true  },
    { { -1.0f,  0.0f,  0.0f }, 0, "X", false },
    { {  0.0f,  1.0f,  0.0f }, 1, "Y", true  },
    { {  0.0f, -1.0f,  0.0f }, 1, "Y", false },
    { {  0.0f,  0.0f,  1.0f }, 2, "Z", true  },
    { {  0.0f,  0.0f, -1.0f }, 2, "Z", false },
};

const ImU32 kNavAxisColor[3] = {
    IM_COL32(238,  86, 100, 255),  // X
    IM_COL32(150, 208,  72, 255),  // Y
    IM_COL32( 74, 144, 236, 255),  // Z
};

// フレームをまたぐ操作状態。ViewportPanel が「ギズモがマウスを取っているか」を
// 前フレームの結果で問い合わせるため、静的に保持する。
struct NavGizmoState {
    bool   hovered     = false;  // ギズモ円内にカーソルがある
    int    hoveredAxis = -1;     // ホバー中のボール (なければ -1)
    bool   pressed     = false;  // ギズモ内で左ボタンを押している
    bool   dragging    = false;  // しきい値を超えて回した (= クリック扱いにしない)
    int    pressedAxis = -1;
    ImVec2 pressPos{};
    float  orbitYaw    = 0.0f;   // ドラッグ中に積む yaw / pitch (度)
    float  orbitPitch  = 0.0f;

    // 軸クリック時の補間。回転を Slerp せず yaw/pitch で補間するのは、
    // DebugCamera が yaw/pitch から姿勢を組み立てる以上、
    // 途中でロール成分が入ると着地時に水平がずれて見えるため。
    bool  animActive = false;
    float animT      = 0.0f;
    float fromYaw = 0.0f, fromPitch = 0.0f;
    float toYaw   = 0.0f, toPitch   = 0.0f;
};

NavGizmoState g_navGizmo;

// 明度と不透明度をまとめて調整する (奥行きフェードとホバー強調で使う)。
ImU32 NavShade(ImU32 color, float brightness, float alpha)
{
    const auto ch = [&](int shift) {
        const float v = static_cast<float>((color >> shift) & 0xFFu) * brightness;
        return static_cast<ImU32>(math::Clamp(v, 0.0f, 255.0f));
    };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT),
                    static_cast<ImU32>(math::Clamp(alpha * 255.0f, 0.0f, 255.0f)));
}

// forward からエンジン規約の yaw / pitch (度) を取り出す。
// WHY: DebugCamera::Teleport と同じ式にしないと、適用後に内部 yaw/pitch がずれて
//      次のオービットで視点が飛ぶ。
void NavForwardToYawPitch(const math::Vector3& fwd, float& yaw, float& pitch)
{
    pitch = math::ToDeg(std::asin(math::Clamp(-fwd.y, -1.0f, 1.0f)));
    yaw   = math::ToDeg(std::atan2(fwd.x, fwd.z));
}

// yaw / pitch からカメラ姿勢を組み立て、ピボットと距離を保ったまま Teleport 要求へ流す。
// 実体は Util/ViewportCamera へ移した (コマンドパレット・AI からの軸ビューと同じ経路)。
void NavApplyYawPitch(EditorContext& ctx, float yaw, float pitch)
{
    PointEditorCamera(ctx, yaw, pitch);
}

// from → to の角度差を -180..180 に畳む (補間で遠回りさせないため)。
float NavShortestAngle(float from, float to)
{
    float d = std::fmod(to - from + 540.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

} // namespace

bool IsOrientationGizmoHovered() { return g_navGizmo.hovered; }
bool IsOrientationGizmoActive()  { return g_navGizmo.pressed || g_navGizmo.animActive; }

void DrawOrientationGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    NavGizmoState& s = g_navGizmo;
    if (!ctx.editorCamera) {
        s = NavGizmoState{};
        return;
    }

    const ImVec2 center  = { viewportMin.x + viewportSize.x - kNavSize * 0.5f - kNavPadding,
                             viewportMin.y + kNavSize * 0.5f + kNavPadding };
    const float  radius  = kNavSize * 0.5f;
    const float  axisLen = radius - kNavBallNearR - 1.0f;

    // --- 各軸をビュー空間へ落として、スクリーン位置と奥行きを求める ---
    // view 行列の 3x3 は R^T (ワールド → ビュー) なので、
    // ワールド軸 e_i のビュー空間表現はその i 列目そのものになる。
    // ビュー空間は x=右 / y=上 / z=奥 (左手系) なので、z がそのまま奥行き値になる。
    const math::Matrix4 view = ctx.editorCamera->GetViewMatrix();

    struct NavBall {
        int    axis  = 0;
        ImVec2 pos{};
        float  depth = 0.0f;  // ビュー空間 z: 大きいほど奥
    };
    NavBall balls[6];
    for (int i = 0; i < 6; ++i) {
        const math::Vector3& d = kNavAxes[i].dir;
        const math::Vector3  v = {
            view.m[0][0] * d.x + view.m[0][1] * d.y + view.m[0][2] * d.z,
            view.m[1][0] * d.x + view.m[1][1] * d.y + view.m[1][2] * d.z,
            view.m[2][0] * d.x + view.m[2][1] * d.y + view.m[2][2] * d.z,
        };
        balls[i].axis  = i;
        balls[i].pos   = { center.x + v.x * axisLen, center.y - v.y * axisLen };
        balls[i].depth = v.z;
    }

    // 奥 → 手前の描画順。手前のボールが奥のボールを覆い隠すようにする。
    int order[6] = { 0, 1, 2, 3, 4, 5 };
    std::sort(order, order + 6,
              [&](int a, int b) { return balls[a].depth > balls[b].depth; });

    // --- ホバー判定 ---
    const ImVec2 mouse = ImGui::GetMousePos();
    const float  mdx   = mouse.x - center.x;
    const float  mdy   = mouse.y - center.y;
    const bool   inRegion = ImGui::IsWindowHovered()
                         && (mdx * mdx + mdy * mdy) <= radius * radius;

    // 重なったボールは手前を優先する (order の後ろほど手前)。
    int hoverAxis = -1;
    if (inRegion && !s.pressed) {
        constexpr float kHitR = kNavBallNearR + 2.0f;
        for (int k = 5; k >= 0; --k) {
            const NavBall& b  = balls[order[k]];
            const float    hx = mouse.x - b.pos.x;
            const float    hy = mouse.y - b.pos.y;
            if (hx * hx + hy * hy <= kHitR * kHitR) { hoverAxis = b.axis; break; }
        }
    }
    s.hovered     = inRegion;
    s.hoveredAxis = s.dragging ? -1 : (s.pressed ? s.pressedAxis : hoverAxis);

    // --- 入力: クリック = 軸へ回り込み / ドラッグ = オービット ---
    // WHY: Alt+左ドラッグは DebugCamera 側のオービットに割り当て済み。ここでも掴むと
    //      同じドラッグで 2 系統の回転が同時に走り、回転量が二重になる。
    if (inRegion && !ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s.pressed     = true;
        s.dragging    = false;
        s.pressedAxis = hoverAxis;
        s.pressPos    = mouse;
        s.animActive  = false;  // 補間中に掴んだら、その場から手動操作へ引き継ぐ
        NavForwardToYawPitch(ctx.editorCamera->GetForward(), s.orbitYaw, s.orbitPitch);
    }

    if (s.pressed) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float mx = mouse.x - s.pressPos.x;
            const float my = mouse.y - s.pressPos.y;
            if (!s.dragging && (mx * mx + my * my) > kNavDragSlop * kNavDragSlop)
                s.dragging = true;
            if (s.dragging) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                s.orbitYaw   += delta.x * kNavOrbitSens;
                s.orbitPitch += delta.y * kNavOrbitSens;
                s.orbitPitch  = math::Clamp(s.orbitPitch, -89.0f, 89.0f);
                NavApplyYawPitch(ctx, s.orbitYaw, s.orbitPitch);
            }
        } else {
            // 離した: 回していなければ「その軸から見る」視点移動を始める。
            if (!s.dragging && s.pressedAxis >= 0) {
                const math::Vector3 targetFwd = kNavAxes[s.pressedAxis].dir * -1.0f;
                float toYaw = 0.0f, toPitch = 0.0f;
                NavForwardToYawPitch(targetFwd, toYaw, toPitch);
                NavForwardToYawPitch(ctx.editorCamera->GetForward(), s.fromYaw, s.fromPitch);
                // 真上・真下は yaw が定まらない。今の向きを保ったまま見下ろす / 見上げる。
                if (kNavAxes[s.pressedAxis].dir.y != 0.0f) toYaw = s.fromYaw;
                s.toYaw      = s.fromYaw + NavShortestAngle(s.fromYaw, toYaw);
                s.toPitch    = math::Clamp(toPitch, -89.9f, 89.9f);
                s.animT      = 0.0f;
                s.animActive = true;
            }
            s.pressed  = false;
            s.dragging = false;
        }
    }

    if (s.animActive) {
        s.animT += ImGui::GetIO().DeltaTime / kNavSnapTime;
        const float t = math::Clamp(s.animT, 0.0f, 1.0f);
        const float e = t * t * (3.0f - 2.0f * t);  // smoothstep: 始点と終点で減速する
        NavApplyYawPitch(ctx,
                         s.fromYaw   + (s.toYaw   - s.fromYaw)   * e,
                         s.fromPitch + (s.toPitch - s.fromPitch) * e);
        if (t >= 1.0f) s.animActive = false;
    }

    // --- 描画 ---
    // 背景の円は描かない (シーンの見通しを塞がないため)。
    // 明るい背景でも埋もれないよう、ボールと軸線それぞれに暗い縁取りを持たせている。
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int k = 0; k < 6; ++k) {
        const NavBall& b  = balls[order[k]];
        const NavAxis& ax = kNavAxes[b.axis];

        // depth は -1..1。奥ほど小さく・暗く描いて前後を判別できるようにする。
        const float far01 = math::Clamp(b.depth * 0.5f + 0.5f, 0.0f, 1.0f);
        const float ballR = kNavBallNearR + (kNavBallFarR - kNavBallNearR) * far01;
        const float fade  = 1.0f - far01 * 0.45f;
        const bool  hot   = (s.hoveredAxis == b.axis);
        const ImU32 base  = kNavAxisColor[ax.colorIndex];

        // 中心からの線は正方向だけに引く (負方向は線を持たないのが Blender の見た目)。
        // 背景円を持たないぶん、暗い縁取りを一本下に敷いて明るいシーン上でも軸線を残す。
        if (ax.positive) {
            const float w = hot ? 3.0f : 2.2f;
            dl->AddLine(center, b.pos, IM_COL32(0, 0, 0, static_cast<int>(90 * fade)), w + 2.0f);
            dl->AddLine(center, b.pos, NavShade(base, hot ? 1.15f : 1.0f, 0.9f * fade), w);
        }

        if (ax.positive || hot) {
            dl->AddCircleFilled(b.pos, ballR, NavShade(base, hot ? 1.2f : 1.0f, fade), 20);
            dl->AddCircle(b.pos, ballR, NavShade(IM_COL32(0, 0, 0, 255), 1.0f, 0.47f * fade),
                          20, 1.5f);
        } else {
            // 負方向は中空。軸色の輪郭 + 暗い塗りで「裏側」を表す。
            dl->AddCircleFilled(b.pos, ballR, NavShade(base, 0.30f, 0.85f * fade), 20);
            dl->AddCircle(b.pos, ballR, NavShade(base, 1.0f, 0.90f * fade), 20, 1.8f);
        }

        // ラベルは正方向を常時、負方向はホバー時だけ (Blender と同じ情報量)。
        // 奥に回って縮んだボールは文字が潰れるので出さない。
        if ((ax.positive || hot) && ballR >= 9.0f) {
            const ImVec2 ts = ImGui::CalcTextSize(ax.label);
            dl->AddText({ b.pos.x - ts.x * 0.5f, b.pos.y - ts.y * 0.5f },
                        NavShade(IM_COL32(18, 18, 20, 255), 1.0f, 0.92f * fade), ax.label);
        }

        // ホバー中のボールは外側にリングを足して、クリック先を明示する。
        if (hot)
            dl->AddCircle(b.pos, ballR + 3.0f, IM_COL32(255, 255, 255, 210), 24, 1.6f);
    }
}

void DrawGizmo(EditorContext& ctx,
               const ImVec2& viewportMin,
               const ImVec2& viewportSize,
               int& lastOp,
               int& lastMode,
               bool& prevOver,
               bool& prevUsing)
{
    scene::EntityID selected = ctx.PrimarySelected();
    if (!selected.IsValid() || !ctx.activeScene || !ctx.editorCamera) return;

    scene::GameObject* go = ctx.activeScene->GetGameObject(selected);
    if (!go) return;


    math::Matrix4 viewCol = ToColumnMajor(ctx.editorCamera->GetViewMatrix());
    math::Matrix4 projCol = ToColumnMajor(ctx.editorCamera->GetProjectionMatrix());

    // ギズモを置くワールド行列を決める。
    //  Pivot  : プライマリの Transform をそのまま使う (従来どおり、行列を直接書き戻せる)
    //  Center : 選択全体のバウンズ中心へ置く。回転成分は Local のときだけプライマリから借り、
    //           スケール成分は持たせない (掴んだ点を基準にした素直な相対変換にするため)
    // WHY: Center ではギズモ行列がどのオブジェクトの Transform とも一致しないため、
    //      「ギズモの移動量」を全対象へ相対適用する経路に一本化する。
    const bool centerPivot = (ctx.gizmoPivot == EditorContext::GizmoPivot::Center);
    math::Matrix4 gizmoWorldRow = go->transform.GetWorldMatrix();
    if (centerPivot) {
        math::Vector3 center{};
        float         radius = 0.0f;
        if (ComputeSelectionBounds(ctx, center, radius)) {
            math::Matrix4 basis = math::Matrix4::Identity();
            if (ctx.gizmoSpace == EditorContext::GizmoSpace::Local)
                basis = math::Matrix4::Rotate(go->transform.rotation);
            basis.m[0][3] = center.x;
            basis.m[1][3] = center.y;
            basis.m[2][3] = center.z;
            gizmoWorldRow = basis;
        }
    }
    math::Matrix4 worldCol = ToColumnMajor(gizmoWorldRow);

    auto& style = ImGuizmo::GetStyle();
    style.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 0.95f, 0.05f, 1.0f);
    style.Colors[ImGuizmo::INACTIVE] = ImVec4(0.75f, 0.75f, 0.75f, 0.75f);
    style.TranslationLineThickness = 4.0f;
    style.RotationLineThickness = 4.0f;
    style.ScaleLineThickness = 4.0f;

    ImGuizmo::SetDrawlist();
    ImGuizmo::Enable(true);
    // ImGuizmo はハンドルの大きさを射影から逆算するため、ここを間違えると
    // 正投影でハンドルが極端に伸び縮みして掴めなくなる。
    ImGuizmo::SetOrthographic(
        ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic);
    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, viewportSize.x, viewportSize.y);

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Rotate) op = ImGuizmo::ROTATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Scale) op = ImGuizmo::SCALE;

    ImGuizmo::MODE mode = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
        ? ImGuizmo::WORLD
        : ImGuizmo::LOCAL;

    const float snapVal = (ctx.gizmoMode == EditorContext::GizmoMode::Rotate) ? ctx.snapRot
                        : (ctx.gizmoMode == EditorContext::GizmoMode::Scale)  ? ctx.snapScale
                        :                                                        ctx.snapPos;
    float snap[3] = { snapVal, snapVal, snapVal };
    const int opInt = static_cast<int>(op);
    const int modeInt = static_cast<int>(mode);
    if (lastOp != opInt || lastMode != modeInt) {
        lastOp = opInt;
        lastMode = modeInt;
    }

    // WHY: Unity と同じく Ctrl 押下中はモーメンタリスナップ (押している間だけスナップ有効)。
    //      設定でスナップ ON のときは常時有効。
    const bool snapActive = ctx.snapEnabled || ImGui::GetIO().KeyCtrl;
    ImGuizmo::Manipulate(
        &viewCol.m[0][0],
        &projCol.m[0][0],
        op,
        mode,
        &worldCol.m[0][0],
        nullptr,
        snapActive ? snap : nullptr);

    const bool gizmoOver = ImGuizmo::IsOver();
    const bool gizmoUsing = ImGuizmo::IsUsing();
    const bool wasUsing = prevUsing;

    // マルチ選択ドラッグ 1 回分の編集状態。instanceIds[0] は必ずプライマリ。
    // before / startWorld / applyDelta は instanceIds と同じ並び。
    struct GizmoEdit {
        std::vector<std::string> instanceIds;
        std::vector<scene::Transform> before;
        std::vector<math::Matrix4> startWorld;
        std::vector<bool> applyDelta;  // true: ギズモの移動量を相対適用する対象
        // ドラッグ開始時のギズモ行列の逆。delta = 現在のギズモ行列 * これ。
        math::Matrix4 gizmoStartWorldInv = math::Matrix4::Identity();
        EditorContext::GizmoMode mode = EditorContext::GizmoMode::Translate;
        // Center ピボット中はギズモ行列がどの Transform とも一致しないため、
        // プライマリにも delta を相対適用する (直接書き戻しはできない)。
        bool centerPivot = false;
        bool active = false;
    };
    static GizmoEdit edit;

    if (gizmoUsing && !wasUsing) {
        edit.instanceIds.clear();
        edit.before.clear();
        edit.startWorld.clear();
        edit.applyDelta.clear();

        // WHY: 親子が同時に選択されている場合、子は親の移動に追従するため、
        //      子にも delta を掛けると二重に動く。「選択済みの祖先を持たない」
        //      top-level オブジェクトにだけ delta を適用する (Unity と同じ規則)。
        auto hasSelectedAncestor = [&ctx](scene::GameObject* obj) {
            for (scene::GameObject* p = obj->GetParent(); p; p = p->GetParent()) {
                if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(),
                              p->GetID()) != ctx.selectedEntities.end())
                    return true;
            }
            return false;
        };

        // プライマリを先頭に登録する。
        // Pivot ピボットではギズモ行列 = プライマリのワールド行列なので直接書き戻す
        // (applyDelta = false)。Center ピボットでは一致しないため delta 適用側へ回す。
        edit.instanceIds.push_back(go->instanceId);
        edit.before.push_back(go->transform);
        edit.startWorld.push_back(go->transform.GetWorldMatrix());
        edit.applyDelta.push_back(centerPivot && !hasSelectedAncestor(go));

        for (scene::EntityID id : ctx.selectedEntities) {
            if (id == selected) continue;
            scene::GameObject* sel = ctx.activeScene->GetGameObject(id);
            if (!sel || ctx.IsLocked(id)) continue;
            edit.instanceIds.push_back(sel->instanceId);
            edit.before.push_back(sel->transform);
            edit.startWorld.push_back(sel->transform.GetWorldMatrix());
            edit.applyDelta.push_back(!hasSelectedAncestor(sel));
        }

        edit.gizmoStartWorldInv = math::Matrix4::Inverse(gizmoWorldRow);
        edit.mode        = ctx.gizmoMode;
        edit.centerPivot = centerPivot;
        edit.active      = true;
    }

    if (gizmoOver != prevOver || gizmoUsing != prevUsing) {
        prevOver = gizmoOver;
        prevUsing = gizmoUsing;
    }

    if (!gizmoUsing) {
        if (wasUsing && edit.active) {
            // ドラッグ終了: 全対象の before/after を 1 コマンドにまとめて Undo 登録する。
            // WHY: マルチ選択の移動を対象ごとに分けると Ctrl+Z を選択数だけ叩く羽目になる。
            scene::Scene* scene = ctx.activeScene;
            const std::vector<std::string> ids = edit.instanceIds;
            const std::vector<scene::Transform> before = edit.before;
            std::vector<scene::Transform> after;
            after.reserve(ids.size());
            bool anyChanged = false;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                scene::GameObject* obj = scene->FindByGuid(ids[i]);
                after.push_back(obj ? obj->transform : before[i]);
                if (obj && !GizmoTransformEquals(before[i], after[i]))
                    anyChanged = true;
            }
            const auto markDirty = ctx.markSceneDirty;
            const char* description =
                edit.mode == EditorContext::GizmoMode::Rotate ? "Rotate GameObject" :
                edit.mode == EditorContext::GizmoMode::Scale ? "Scale GameObject" :
                                                               "Move GameObject";
            if (ctx.undoStack && anyChanged) {
                auto applyAll = [scene, ids, markDirty](const std::vector<scene::Transform>& values) {
                    for (std::size_t i = 0; i < ids.size(); ++i)
                        if (auto* target = scene->FindByGuid(ids[i]))
                            target->transform = values[i];
                    if (markDirty) markDirty();
                };
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    description,
                    [applyAll, after]() { applyAll(after); },
                    [applyAll, before]() { applyAll(before); }));
            }
            if (anyChanged && ctx.markSceneDirty)
                ctx.markSceneDirty();
            edit.active = false;
        }
        return;
    }

    const math::Matrix4 worldRow = math::Matrix4::Transpose(worldCol);

    // ギズモの移動量 (delta) を、選択済みの祖先を持たない対象へ相対適用する。
    // WHY: 添字 0 (プライマリ) より先に他オブジェクトへ適用する。選択中の「親」が動いた
    //      後にプライマリのワールド行列を書き込むことで、プライマリはギズモ位置へ一致する。
    //      Center ピボットではプライマリも delta 側に含まれるため、この順序で問題ない。
    if (edit.active) {
        const math::Matrix4 delta = worldRow * edit.gizmoStartWorldInv;
        for (std::size_t i = edit.instanceIds.size(); i-- > 0; ) {
            if (!edit.applyDelta[i]) continue;
            if (auto* other = ctx.activeScene->FindByGuid(edit.instanceIds[i]))
                ApplyWorldRowToTransform(*other, delta * edit.startWorld[i]);
        }
    }

    // Pivot ピボットのみ: ギズモ行列はプライマリのワールド行列そのものなので、
    // 丸め誤差を挟まず直接書き戻す。
    if (!edit.centerPivot)
        ApplyWorldRowToTransform(*go, worldRow);
}

} // namespace fbzz::editor
