/// @file DecalComponent.hpp
/// @brief デカール投影コンポーネント
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// Transform の OBB をプロキシとして深度バッファからワールド座標を復元し、
/// テクスチャを投影する。lifetime < 0 で永続、>= 0 で時間経過によりフェードアウト→削除。
#pragma once
#include <Physics/Layer.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

struct DecalComponent {
    bool enabled = true;

    // ── マテリアル ─────────────────────────────────────────────────────────────
    // .mat への参照。空なら組み込みの Decal.hlsl と下のテクスチャ / 色で描く。
    //
    // WHY テクスチャパスと別に持つか:
    //   着弾痕や血痕は「テクスチャを 1 枚貼って薄める」だけの用途が圧倒的多数で、
    //   そこへ .mat の割り当てを必須にすると、弾痕を 1 種類足すたびにアセットが
    //   1 つ増える。マテリアルは「組み込みでは描けない絵」を作りたいときの上乗せにする。
    //
    // WHY .mat を使い回すか (デカール専用形式を作らないか):
    //   シェーダー・テクスチャ・ブレンドという中身はメッシュ材質と同じで、違うのは
    //   投影の仕方だけ。形式を分けると Inspector もリフレクションもサムネイルも
    //   二重に持つことになる。render_path = "decal" で用途だけ区別する。
    std::string materialPath;

    // ── テクスチャ (materialPath が空のときだけ使う) ──────────────────────────
    std::string albedoTexPath;      // t0
    std::string normalTexPath;      // t1
    std::string emissiveTexPath;    // t3

    // ── サーフェスパラメータ (materialPath が空のときだけ使う) ─────────────────
    float albedoColor[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    float normalStrength   = 1.0f;

    // ── 角度フェード ─────────────────────────────────────────────────────────
    // 受け面の法線が投影軸から傾くほどデカールを薄くする。
    // WHY: OBB 投影は投影軸に対して斜めな面へ当てるとテクスチャが引き伸ばされ、
    //      長い筋になる。着弾痕や血痕が壁と床の角をまたいだ瞬間に
    //      「伸びた汚れ」として露見する、デカールで最も目立つ破綻がこれ。
    //      角度で薄めれば、破綻する範囲がそのまま消える。
    //
    // WHY .mat 側へ移さないか: 投影ボリュームの形と一体で決まる値で、同じ .mat を
    //      壁用と床用で使い回すときに別々に調整したくなる。
    float angleFadeStrength = 1.0f;  // 0 = フェードなし
    float angleFadeDegrees  = 70.0f; // これ以上寝た面では完全に消える [0, 89]

    // ── エミッシブ (materialPath が空のときだけ使う) ───────────────────────────
    float emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
    float emissiveScale    = 0.0f;

    // ── ライフタイム ─────────────────────────────────────────────────────────
    float lifetime = -1.0f;  // < 0 で永続
    float fadeTime = 1.0f;   // 消える前のフェード時間 (秒)
    float age      = 0.0f;   // DecalPass が毎フレーム加算する

    // 不透明度の倍率 [0, 1]。ライフタイムフェードと同じ場所 (DecalCB::alpha) へ掛かる。
    //
    // WHY albedoColor[3] と別に要るか:
    //   VFX のフェードカーブやスクリプトが「今どれだけ濃いか」を毎フレーム書く先が要る。
    //   albedoColor は組み込み経路の値なので、.mat を割り当てた瞬間に効かなくなり、
    //   カーブで作った減り方が黙って消える。マテリアルの中身を知らずに掛けられる倍率は
    //   投影側 (DecalCB) にしか置けない。
    //
    // WHY シーンへ保存しないか:
    //   毎フレーム動く量で、保存すると Play を止めた瞬間の値がシーン差分として残る。
    //   オーサリング時の濃さは albedoColor / .mat の tint が正本。
    float opacity  = 1.0f;

    // ── レイヤーフィルタ ──────────────────────────────────────────────────────
    // デカールを受け取るレイヤーマスク。このマスクに含まれない GameObject には投影しない。
    fbzz::LayerMask receiverLayerMask = fbzz::Layer::Everything;

    // ── マテリアルのこのデカールだけの上書き (ランタイム専用) ──────────────────
    //
    // WHY デカールごとに持てるようにするか:
    //   .mat は参照する全デカールが共有する実体で、ここへ書くと 1 発の弾痕を
    //   濃くしたつもりが同じ .mat の弾痕すべてに乗る。かといって値違いのために
    //   .mat を弾痕の数だけ作ると、色を 1 つ直すのに全ファイルを開くことになる。
    //   共有は .mat、差分はここ、と分ける。
    //
    // WHY シーンへ保存しないか:
    //   ここへ入る値は着弾ごとに変わる量で、保存すると Play を止めた瞬間の値が
    //   シーンの差分として残り続ける。オーサリング時の見た目は .mat が正本。
    //   スクリプトからは decal.SetMaterialFloat() などで書く。
    std::unordered_map<std::string, std::vector<float>> materialParamOverrides;
    std::unordered_map<std::string, std::string>        materialTextureOverrides;

    const char* GetTypeName() const { return "Decal"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",         enabled);
        r.Field("materialPath",    materialPath);
        r.Tooltip("デカールマテリアル (.mat)。空欄で組み込みのテクスチャ投影。"
                  "render_path = \"decal\" のものだけが使えます");
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
