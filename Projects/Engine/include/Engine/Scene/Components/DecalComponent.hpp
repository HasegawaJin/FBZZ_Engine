// FBZZ Engine
// DecalComponent.hpp | fbzz::scene
// デカール投影コンポーネント
// Transform の OBB をプロキシとして深度バッファからワールド座標を復元し、
// テクスチャを投影する。Albedo / Normal / Emissive の 3 チャンネルに対応する。
// lifetime < 0 で永続、>= 0 で時間経過によりフェードアウト→削除される。
#pragma once
#include <Physics/Layer.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

struct DecalComponent {
    bool enabled = true;

    // ── テクスチャ ──────────────────────────────────────────────────────────
    std::string albedoTexPath;      // t0
    std::string normalTexPath;      // t1
    std::string emissiveTexPath;    // t3

    // ── サーフェスパラメータ ───────────────────────────────────────────────
    float albedoColor[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    float normalStrength   = 1.0f;

    // ── 角度フェード ─────────────────────────────────────────────────────────
    // 受け面の法線が投影軸から傾くほどデカールを薄くする。
    // WHY: OBB 投影は投影軸に対して斜めな面へ当てるとテクスチャが引き伸ばされ、
    //      長い筋になる。着弾痕や血痕が壁と床の角をまたいだ瞬間に
    //      「伸びた汚れ」として露見する、デカールで最も目立つ破綻がこれ。
    //      角度で薄めれば、破綻する範囲がそのまま消える。
    float angleFadeStrength = 1.0f;  // 0 = フェードなし
    float angleFadeDegrees  = 70.0f; // これ以上寝た面では完全に消える [0, 89]

    // ── エミッシブ ───────────────────────────────────────────────────────────
    float emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
    float emissiveScale    = 0.0f;

    // ── ライフタイム ─────────────────────────────────────────────────────────
    float lifetime = -1.0f;  // < 0 で永続
    float fadeTime = 1.0f;   // 消える前のフェード時間 (秒)
    float age      = 0.0f;   // RenderSystem が毎フレーム加算する

    // ── レイヤーフィルタ ──────────────────────────────────────────────────────
    // デカールを受け取るレイヤーマスク。このマスクに含まれない GameObject には投影しない。
    // デフォルトはすべてのレイヤー (Layer::Everything)。
    fbzz::LayerMask receiverLayerMask = fbzz::Layer::Everything;

    const char* GetTypeName() const { return "Decal"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",         enabled);
        r.Field("albedoTexPath",   albedoTexPath);
        r.Field("normalTexPath",   normalTexPath);
        r.Field("emissiveTexPath", emissiveTexPath);
        r.Field("albedoR",         albedoColor[0]);
        r.Field("albedoG",         albedoColor[1]);
        r.Field("albedoB",         albedoColor[2]);
        r.Field("albedoA",         albedoColor[3]);
        r.Field("normalStrength",  normalStrength);
        r.Field("angleFadeStrength", angleFadeStrength);
        r.Field("angleFadeDegrees",  angleFadeDegrees);
        r.Field("emissiveR",       emissiveColor[0]);
        r.Field("emissiveG",       emissiveColor[1]);
        r.Field("emissiveB",       emissiveColor[2]);
        r.Field("emissiveScale",   emissiveScale);
        r.Field("lifetime",  lifetime);
        r.Field("fadeTime",  fadeTime);
        r.Field("age",       age);
        // IReflector は uint32_t 非対応のため int 経由でシリアライズする。
        // ~0u (-1) も含め正しくラウンドトリップする。
        { int v = static_cast<int>(receiverLayerMask);
          r.Field("receiverLayerMask", v);
          receiverLayerMask = static_cast<fbzz::LayerMask>(static_cast<uint32_t>(v)); }
    }
};

} // namespace fbzz::scene
