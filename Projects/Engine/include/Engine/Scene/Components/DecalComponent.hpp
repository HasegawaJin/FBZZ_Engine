/// @file    DecalComponent.hpp
/// @brief   デカール投影コンポーネント
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note Transform の OBB をプロキシとして深度バッファからワールド座標を復元し、
/// @note テクスチャを投影する。lifetime < 0 で永続、>= 0 で時間経過によりフェードアウト→削除。
#pragma once
#include <Physics/Layer.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

struct DecalComponent {
    bool enabled = true;
    uint64_t lastExtractionFrame = UINT64_MAX;

    /// @name マテリアル
    /// @{
    /// @note .mat への参照。空なら組み込みの Decal.hlsl と下のテクスチャ / 色で描く。
    ///
    /// @note 着弾痕・血痕は「テクスチャを 1 枚貼って薄める」用途が大半なので、.mat を必須に
    /// @note せず組み込みで描ける。.mat はメッシュ材質と同じ形式を使い回し、
    /// @note render_path = "decal" で用途だけ区別する (専用形式にすると二重管理になる)。
    std::string materialPath;
    /// @}

    /// @name テクスチャ (materialPath が空のときだけ使う)
    /// @{
    std::string albedoTexPath;      ///< @note t0
    std::string normalTexPath;      ///< @note t1
    std::string emissiveTexPath;    ///< @note t3
    /// @}

    /// @name サーフェスパラメータ (materialPath が空のときだけ使う)
    /// @{
    float albedoColor[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    float normalStrength   = 1.0f;
    /// @}

    /// @name 角度フェード
    /// @{
    /// @note 受け面の法線が投影軸から傾くほどデカールを薄くする。
    /// @note OBB 投影は斜めな面でテクスチャが伸び、壁と床の角をまたぐ痕として露見する
    /// @note (最も目立つ破綻)。投影ボリュームと一体で決まる値なので .mat 側には置かず、
    /// @note 壁用・床用で別々に調整できるようにする。
    float angleFadeStrength = 1.0f;  ///< @note 0 = フェードなし
    float angleFadeDegrees  = 70.0f; ///< @note これ以上寝た面では完全に消える [0, 89]
    /// @}

    /// @name エミッシブ (materialPath が空のときだけ使う)
    /// @{
    float emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
    float emissiveScale    = 0.0f;
    /// @}

    /// @name ライフタイム
    /// @{
    float lifetime = -1.0f;  ///< @note < 0 で永続
    float fadeTime = 1.0f;   ///< @note 消える前のフェード時間 (秒)
    float age      = 0.0f;   ///< @note DecalPass が毎フレーム加算する

    /// @note 出現時のフェード時間 [秒]。0 で «いきなり全濃度» (既定 = 従来の挙動)。
    ///
    /// @note 永続デカール (lifetime < 0) でも効く。DecalPass は fadeInTime > 0 か
    /// @note フリップブックが有効な永続デカールに限り age を進める (常に進めると
    /// @note 編集中に age がシーン差分として増え続ける)。
    float fadeInTime = 0.0f;

    /// @note 不透明度の倍率 [0, 1]。ライフタイムフェードと同じ場所 (DecalCB::alpha) へ掛かる。
    ///
    /// @note albedoColor は .mat 割り当て時に効かなくなるため、VFX カーブやスクリプトが
    /// @note 毎フレーム書く先として投影側 (DecalCB) にこの倍率を持つ。毎フレーム動く量なので
    /// @note シーンへは保存しない (オーサリング時の濃さは albedoColor / .mat の tint が正本)。
    float opacity  = 1.0f;
    /// @}

    /// @name フリップブック (コマ送り)
    /// @{
    /// @note 1 枚のアトラスへ横並びに詰めたコマを時間で進める «痕そのものが変化する» 絵。
    /// @note コマ番号は共有実体の .mat ではなく投影側 (DecalCB) に置く
    /// @note (.mat に置くと同じ .mat の痕が全部同じコマになる)。
    /// @note frameCount <= 1 で無効 (UV は素通し = 従来と同じ絵)。
    int   frameCount   = 0;
    /// @note アトラスの横方向のコマ数。縦の段数は frameCount から切り上げで決まる。
    int   framesPerRow = 1;
    /// @note [fps]。0 なら «寿命いっぱいで 1 周» する (lifetime < 0 では進まない)。
    ///
    /// @note 0 は「消えるまでに終わる」動きを表す。秒数指定だと lifetime を触るたびに
    /// @note fps を計算し直すことになる。
    float frameRate    = 0.0f;
    /// @note 最後のコマまで行ったあと先頭へ戻る。false なら最後のコマで止まる (既定)。
    /// @note 焦げ・傷は «進んで止まる»、電気の走る痕は «回り続ける»。
    bool  frameLoop    = false;
    /// @}

    /// @name 重なりの前後
    /// @{
    /// @note 小さいほど先に描く (= 後ろになる)。同値なら GameObject の走査順のまま。
    ///
    /// @note デカールは深度を書かず順番どおり合成するだけなので、これが無いと重なる痕の
    /// @note 前後が Hierarchy の並び順でしか決まらず「血だまりの上に足跡」を作れなかった。
    int sortOrder = 0;
    /// @}

    /// @name レイヤーフィルタ
    /// @{
    /// @note デカールを受け取るレイヤーマスク。このマスクに含まれない GameObject には投影しない。
    fbzz::LayerMask receiverLayerMask = fbzz::Layer::Everything;
    /// @}

    /// @name マテリアルのこのデカールだけの上書き (ランタイム専用)
    /// @{
    /// @note .mat は全デカールが共有する実体なので、個体差分はここに持つ (共有は .mat、
    /// @note 差分はここ)。着弾ごとに変わる量なのでシーンへは保存しない。
    /// @note スクリプトからは decal.SetMaterialFloat() などで書く。
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
        r.Field("lifetime",   lifetime);
        r.Field("fadeTime",   fadeTime);
        r.Field("fadeInTime", fadeInTime);
        r.Field("age",        age);
        r.Field("frameCount",   frameCount);
        r.Field("framesPerRow", framesPerRow);
        r.Field("frameRate",    frameRate);
        r.Field("frameLoop",    frameLoop);
        r.Field("sortOrder",    sortOrder);
        /// @note IReflector は uint32_t 非対応のため int 経由でシリアライズする。
        /// @note ~0u (-1) も含め正しくラウンドトリップする。
        { int v = static_cast<int>(receiverLayerMask);
          r.Field("receiverLayerMask", v);
          receiverLayerMask = static_cast<fbzz::LayerMask>(static_cast<uint32_t>(v)); }
    }
    /// @}
};

} /// @note namespace fbzz::scene
