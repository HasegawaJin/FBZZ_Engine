/// @file    SerpentSlamVfxComponent.hpp
/// @brief   FX_SRP_Slam.vfx の公開パラメーター。11 m の胴が落ちて床を «帯» で叩いた
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// WHY 締め上げ (FX_SRP_Snap) と分けるか:
///   以前はどちらも «中心に爆発 1 発 + 両側に土煙 2 発» で、向きの意味まで同じだった。
///   叩きつけは «線» が落ちてくる手で、危険なのは 2 つの口を結ぶ帯そのもの。
///   締め上げは «角» が寄る手で、危険なのは角から外へ広がる側。同じ絵で出すと、
///   予兆のデカールを見ていないと 2 つを区別する手掛かりが 1 つも無くなる。
///
/// WHY 1 発を «長い» 層で作るか (点を並べないか):
///   呼ぶ側が 11 m ぶんの点を撒くと、枠 (VfxManager の Pool) をその数だけ食う。
///   帯は 1 枚の水平ビルボードを弦の長さまで伸ばせば済み、粒はその上に散らすだけでよい。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

#include <algorithm>
#include <string_view>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentSlamVfxComponent : public Script {
    FBZZ_SCRIPT(SerpentSlamVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(dustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.55f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。刀の赤青は乗せない (土は誰が立てても同じ色)")

    FBZZ_GROUP("Band")
    FBZZ_FIELD_RANGE(float, bandLength, 11.0f, "長さ", 1.0f, 24.0f)
    FBZZ_TOOLTIP("叩いた帯の長さ [m]。弧の 2 つの足元の間隔をそのまま入れる。"
                 "層は «ローカル +Z が帯の向き» で置いてあるので、ここは Z を伸ばす")
    FBZZ_FIELD_RANGE(float, bandWidth, 2.6f, "幅", 0.2f, 8.0f)
    FBZZ_TOOLTIP("帯の幅 [m]。当たり判定 (Slam > Radius) と同じ値にすること ─ "
                 "煙の縁で当たると «掠っただけ» が全部当たりになる")
    FBZZ_FIELD_RANGE(float, kickSpeed, 5.5f, "蹴り上げ", 0.0f, 16.0f)
    FBZZ_TOOLTIP("帯の «両脇» へ押し出される速さ [m/s]。落ちてきた胴が空気を"
                 "横へ逃がす向きなので、前後ではなく左右へ配る")
    FBZZ_FIELD_RANGE(float, gritPower, 1.8f, "Grit", 0.0f, 4.0f)

    FBZZ_GROUP("接地")
    FBZZ_FIELD_RANGE(float, scorchLength, 11.0f, "Scorch Length", 1.0f, 24.0f)
    FBZZ_TOOLTIP("床に残る «擦った跡» の長さ [m]。帯と同じにすると «そこに落ちた» が残る")
    FBZZ_FIELD_RANGE(float, thudLight, 6.0f, "着地音のライト", 0.0f, 30.0f)
    FBZZ_TOOLTIP("落ちた瞬間の弱い光。閃光にしないこと ─ 叩きつけは暗く重い出来事")

    /// フィールドの現在値を配下の層へ書き込む。鳴らす側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(SerpentSlamVfxComponent)

inline void SerpentSlamVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    const float length = (std::max)(bandLength, 0.5f);
    const float width  = (std::max)(bandWidth, 0.1f);
    const float kick   = (std::max)(kickSpeed, 0.0f);

    vfxbind::ParticleColor(root, "Band Smoke", dustColor);
    vfxbind::ParticleColor(root, "Wing Left",  dustColor);
    vfxbind::ParticleColor(root, "Wing Right", dustColor);
    vfxbind::ParticleColor(root, "Grit", Vector4{ dustColor.x, dustColor.y, dustColor.z, 1.0f });

    // 球では胴の長さぶん左右・地下にも湧く。接地線に沿う薄い箱へ限定する。
    constexpr const char* nodes[] = { "Band Smoke", "Grit", "Wing Left", "Wing Right" };
    for (const char* node : nodes) {
        if (GameObject* target = vfxbind::Find(root, node)) {
            if (auto* emitter = target->GetComponent<ParticleEmitter>()) {
                emitter->settings.shape = ParticleEmitterShape::Box;
                const bool edge = std::string_view(node) == "Wing Left"
                               || std::string_view(node) == "Wing Right";
                emitter->settings.boxExtents = { edge ? 0.12f : width * 0.5f, 0.08f, length * 0.5f };
            }
        }
    }
    vfxbind::NodePosition(root, "Wing Left", { -width * 0.5f, 0.12f, 0.0f });
    vfxbind::NodePosition(root, "Wing Right", { width * 0.5f, 0.12f, 0.0f });
    vfxbind::ParticleSizeEnd(root, "Band Smoke", width * 1.1f);

    // 脇へ逃がす。左右で符号だけが違う ─ 同じ向きにすると «横へ滑った» になる。
    vfxbind::ParticleEmitVelocity(root, "Wing Left",  { -kick, kick * 0.30f, 0.0f });
    vfxbind::ParticleEmitVelocity(root, "Wing Right", {  kick, kick * 0.30f, 0.0f });
    vfxbind::ParticleEmitVelocity(root, "Grit",       { 0.0f, kick * 0.95f, 0.0f });
    vfxbind::ParticleVelocitySpread(root, "Grit", (std::max)(gritPower, 0.0f));
    vfxbind::ParticleSizeEnd(root, "Wing Left",  width * 1.2f);
    vfxbind::ParticleSizeEnd(root, "Wing Right", width * 1.2f);

    if (GameObject* scorch = vfxbind::Find(root, "Scorch Band")) {
        const Vector3 current = scorch->transform.scale;
        scorch->transform.scale =
            Vector3{ width * 1.15f, current.y, (std::max)(scorchLength, 0.5f) };
    }

    vfxbind::LightIntensity(root, "Thud Light", thudLight);
}

} // namespace sandbox
