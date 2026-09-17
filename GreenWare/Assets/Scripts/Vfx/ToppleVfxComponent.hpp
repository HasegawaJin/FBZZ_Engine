/// @file    ToppleVfxComponent.hpp
/// @brief   FX_BOSS_Topple.vfx の公開パラメーター。崩しが満ちてボスが倒れた «ドンッ»
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note 衝突の爆発 (FX_IMP_Explosion) は流用しない。爆発は閃光と熱が主役だが、転倒は
///       «重機が床に落ちた» で主役は床の側 (走る輪・土煙・ひび)。明るさで重さを出すと
///       企画書 12.2 (発光の白飛び) にぶつかるため、光は «影が一瞬動く» 程度に絞る。
/// @note 極性色は乗せない。転倒は極の出来事ではなく (break-parry.md)、土煙・砂・破片は
///       12.2 の無彩色側。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ToppleVfxComponent : public Script {
    FBZZ_SCRIPT(ToppleVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(dustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.55f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。刀の赤青は乗せない (土は誰が立てても同じ色)。アルファが土煙の濃さ")
    FBZZ_FIELD_RANGE(float, scale, 1.0f, "スケール", 0.3f, 3.0f)
    FBZZ_TOOLTIP("全体の大きさ。1.0 でポラリティ・コア (6m 級)。サーペントは節の太さに合わせて下げる")
    FBZZ_FIELD_RANGE(float, crackSize, 9.0f, "亀裂の大きさ", 2.0f, 20.0f)
    FBZZ_TOOLTIP("床のひび (デカール) の直径 [m]")
    FBZZ_FIELD_RANGE(float, thudLight, 8.0f, "着地音のライト", 0.0f, 30.0f)
    FBZZ_TOOLTIP("落ちた瞬間の弱い光。閃光にしないこと ─ 転倒は暗い出来事")

    /// フィールドの現在値を配下の層へ書き込む。撃つ側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(ToppleVfxComponent)

inline void ToppleVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    const float s = (std::max)(scale, 0.05f);

    vfxbind::ParticleColor(root, "Dust Wall", Vector4{ dustColor.x, dustColor.y, dustColor.z, dustColor.w * 0.65f });
    vfxbind::ParticleColor(root, "Dust Settle", Vector4{ dustColor.x, dustColor.y, dustColor.z, dustColor.w * 0.4f });
    /// @note 砂粒は «物» なので不透明のまま (RunDustVfxComponent と同じ理由)。
    vfxbind::ParticleColor(root, "Grit", Vector4{ dustColor.x, dustColor.y, dustColor.z, 1.0f });

    /// @note 大きさに効く層だけ拡縮する。数 (bursts) は変えない ─ 枚数が増えると重なりで白く抜ける。
    vfxbind::ParticleSizeEnd(root, "Ground Ring", 14.0f * s);
    vfxbind::ParticleSizeEnd(root, "Dust Wall",   3.8f * s);
    vfxbind::ParticleSizeEnd(root, "Dust Column", 3.2f * s);
    vfxbind::ParticleSizeEnd(root, "Dust Settle", 3.6f * s);
    vfxbind::ParticleSphereRadius(root, "Dust Wall",   1.6f * s);
    vfxbind::ParticleSphereRadius(root, "Dust Settle", 3.5f * s);
    vfxbind::ParticleSphereRadius(root, "Grit",   1.4f * s);
    vfxbind::ParticleSphereRadius(root, "Shards", 1.6f * s);

    if (GameObject* crater = vfxbind::Find(root, "Crater")) {
        const float d = (std::max)(crackSize, 0.5f);
        crater->transform.scale = Vector3{ d, crater->transform.scale.y, d };
    }

    vfxbind::LightIntensity(root, "Thud Light", thudLight);
}

} // namespace sandbox
