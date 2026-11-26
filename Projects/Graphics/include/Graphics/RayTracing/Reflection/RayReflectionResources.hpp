/// @file    RayReflectionResources.hpp
/// @brief   一段反射のビュー資源と行列反転を使わない GPU カメラ定数。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/RayTracing/Scene/RayGeometryCache.hpp>
#include <Graphics/RayTracing/PathTracing/RayPathScene.hpp>
#include <Graphics/RayTracing/Scene/RayEnvironment.hpp>
#include <Graphics/RayTracing/Reflection/RayReflectionReconstructionResources.hpp>
#include <Graphics/RayTracing/Reflection/RayReflectionMediumCache.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <cstddef>

namespace fbzz::renderer {

/// @note RayReflection.cs.hlsl の b0 と一致。alpha0=invalid、1=opaque specular、2=dielectric full radiance。
/// @note cameraRight/up.w は半投影幅/高さ。near/far と Reversed-Z 深度からワールド位置を復元する。
struct RayReflectionConstants {
    math::Vector4 cameraPosition; ///< カメラ原点のワールド位置を渡すベクトル。
    math::Vector4 cameraRight; ///< ワールド右方向と w の半投影幅。
    math::Vector4 cameraUp; ///< ワールド上方向と w の半投影高。
    math::Vector4 cameraForward; ///< 位置復元に用いるカメラのワールド前方方向。
    math::Vector4 lightDirection; ///< xyz は既定の方向光のワールド方向、w は影の強さ。
    math::Vector4 lightColorIntensity; ///< xyz は方向光の色、w は光の強度。
    math::Vector4 ambientRadiance; ///< 反射照明で使う環境光の線形 RGB。
    uint32_t width = 0; ///< 反射結果を合成するシーンの幅 [px]。
    uint32_t height = 0; ///< 反射結果を合成するシーンの高さ [px]。
    uint32_t instanceCount = 0; ///< ヒット表で参照できるインスタンス数。
    uint32_t orthographic = 0; ///< 平行投影として位置復元を行うか。
    float nearDistance = 0; ///< カメラ近面までの距離 [m]。
    float farDistance = 0; ///< カメラ遠面までの距離 [m]。
    float jitterNdcX = 0; ///< カメラの射影ジッターの X 成分 [NDC]。
    float jitterNdcY = 0; ///< カメラの射影ジッターの Y 成分 [NDC]。
    uint32_t sampleCount = 4; ///< 反射を求めるときの画素当たりのサンプル数。
    uint32_t frameIndex = 0; ///< 反射サンプル系列を変える描画フレーム番号。
    uint32_t incomplete = 0; ///< レイシーンの形状または表面に未収集の部分があるか。
    uint32_t iblReady = 0; ///< 反射照明で使う IBL 資源が利用可能か。
    float maxDistance = 0; ///< 反射レイを打ち切る最大距離 [m]。
    uint32_t writeMetadata = 0; ///< 後段の再構成に渡す画素別ガイドも書くか。
    uint32_t diffuseIndirectEnabled = 0; ///< 共有の拡散間接光入力を反射先の照明で使うか。
    uint32_t traceDistanceLimited = 0; ///< 品質設定に応じた反射距離の制限を適用するか。
    uint32_t emitterCount = 0; ///< 発光三角形の GPU 表に含まれるレコード数。
    uint32_t deltaLightCount = 0; ///< 点・スポット・方向光の GPU 表に含まれるレコード数。
    uint32_t shapeCount = 0; ///< 球・カプセル光源の GPU 表に含まれるレコード数。
    uint32_t sceneLighting = 0; ///< 既定の方向光ではなくシーン光源と環境を使うか。
    uint32_t environmentTableCount = 0; ///< 環境方向の GPU サンプリング表のレコード数。
    uint32_t environmentFaceSize = 0; ///< 環境サンプリング表のキューブ 1 面の幅 [texel]。
    float environmentRotation = 0; ///< 環境キューブのワールド Y 軸回転 [rad]。
    float environmentIntensity = 1; ///< 生の環境放射輝度に掛ける倍率。
    uint32_t environmentMode = 0; ///< 環境放射輝度の取得方式。0 は黒、1 は定数、2 は生キューブ。
    float environmentRadiance[3]{}; ///< 環境モードが定数の場合のシーン線形 RGB 放射輝度。
    /// @note SCREEN_FIRST は十分な SSR confidence の画素を trace 前に除外する。
    uint32_t reflectionResolveEnabled = 0; ///< 画面空間の結果とレイ反射を解決する経路が有効か。
    uint32_t reflectionSsrEnabled = 0; ///< 十分に確かな SSR の結果で反射トレースを省けるか。
    uint32_t glassEnabled = 0; ///< 対応済みの誘電体の反射と透過を計算するか。
    uint32_t glassBoundaryLimit = 0; ///< 一つのレイ輸送で評価するガラス境界数の上限。
    /// @note 1 は現 perspective camera origin が検証済み全 solid bounds 外。0 は従来の画素別媒体探索。
    uint32_t cameraOriginProvenAir = 0; ///< 透視カメラの原点が全検証済み中実形状の外側と証明されたか。
    /// @note bit0 は Hybrid 専用 AS の opaque/cull 証明、bit1 は smooth glass の単一 Fresnel 分岐。
    uint32_t hybridPolicyFlags = 0; ///< Hybrid の専用 AS 被覆証明と Fresnel 分岐を選ぶビット集合。
    /// @note Scene width/height は変更せず、粗い opaque 輸送だけ ceil(Scene/2) の独立資源を使う。
    uint32_t traceWidth = 0; ///< 反射トレースが書く画像の幅 [px]。
    uint32_t traceHeight = 0; ///< 反射トレースが書く画像の高さ [px]。
};
static_assert(sizeof(RayReflectionConstants) == 256);
static_assert(offsetof(RayReflectionConstants, cameraOriginProvenAir) == 240);
static_assert(offsetof(RayReflectionConstants, hybridPolicyFlags) == 244);
static_assert(offsetof(RayReflectionConstants, traceWidth) == 248);
static_assert(offsetof(RayReflectionConstants, traceHeight) == 252);

struct RayReflectionLightingResources {
    ResourceHandle<StructuredBufferTag> emitters; ///< 直接光としてサンプリングする発光三角形の GPU バッファ。
    ResourceHandle<StructuredBufferTag> deltaLights; ///< 点・スポット・方向光の GPU バッファ。
    ResourceHandle<StructuredBufferTag> shapes; ///< 球・カプセル光源の GPU バッファ。
    ResourceHandle<StructuredBufferTag> environmentTable; ///< 環境方向を重点サンプリングするための GPU 分布表。
    ResourceHandle<TextureTag> environment; ///< 放射輝度を読む生の環境キューブのハンドル。
    uint32_t emitterCount = 0; ///< `emitters` で読める発光三角形のレコード数。
    uint32_t deltaLightCount = 0; ///< `deltaLights` で読める光源レコード数。
    uint32_t shapeCount = 0; ///< `shapes` で読める形状光源レコード数。
    uint32_t environmentTableCount = 0; ///< `environmentTable` で読めるレコード数。
    uint32_t environmentFaceSize = 0; ///< 環境サンプリング表のキューブ 1 面の幅 [texel]。
    float environmentRotation = 0; ///< 環境キューブのワールド Y 軸回転 [rad]。
    float environmentIntensity = 1; ///< 生の環境放射輝度に掛ける倍率。
    bool sceneLighting = false; ///< 反射照明へシーン光源・環境のセットを渡すか。
    bool constantEnvironmentKnown = false; ///< 定数環境の放射輝度を検証できたか。
    math::Vector3 constantEnvironmentRadiance; ///< 検証済みの定数環境のシーン線形 RGB 放射輝度。
    bool glassEnabled = false; ///< 誘電体の輸送を実行できる入力が揃っているか。
    bool diffuseIndirectEnabled = false; ///< 共有の拡散間接光を反射先で評価できるか。
    bool cameraOriginProvenAir = false; ///< 透視カメラ原点が中実媒質の境界外と検証されたか。
    math::Vector3 provenAirOrigin; ///< 空気中にあることを証明した原点のワールド位置 [m]。
};

/// @note output と guide/history は Scene extent、halfRaw は粗い opaque 輸送だけの RGBA16F。scene/gpu は同じ静的表面版を指す。
struct RayReflectionViewResources {
    ResourceHandle<TextureTag> output; ///< シーン解像度の反射結果を出力するテクスチャ。
    ResourceHandle<TextureTag> halfRaw; ///< 下げた解像度で生の粗い不透明反射を書き出すテクスチャ。
    ResourceHandle<ConstantBufferTag> constants; ///< 反射トレースのビュー・照明・品質入力を渡す定数バッファ。
    uint32_t width = 0; ///< シーン解像度の出力幅 [px]。
    uint32_t height = 0; ///< シーン解像度の出力高 [px]。
    /// @note 再構成・半解像度資源の準備失敗時は divisor1 と Scene extent に戻す。
    uint32_t traceWidth = 0; ///< 生の反射トレース資源の幅 [px]。
    uint32_t traceHeight = 0; ///< 生の反射トレース資源の高さ [px]。
    uint32_t resolutionDivisor = 1; ///< 生の反射トレースをシーン解像度から下げる除数。
    RayScene scene; ///< 反射対象の形状と表面を収集した CPU レイシーン。
    RaySceneGpu gpu; ///< `scene` と同じ形状・表面版の TLAS と GPU レコード。
    RayPathSceneBuilder sceneBuilder; ///< 積分用の形状読み出しと内容版を保持する反射用ビルダー。
    RayReflectionMediumCache mediumCache; ///< 中実誘電体の境界外にカメラ原点があるか検証するキャッシュ。
    RayPathScene pathScene; ///< 反射先で積分する光源と環境を保持する CPU スナップショット。
    ResourceHandle<StructuredBufferTag> emitters; ///< GPU に転送した発光三角形のバッファ。
    ResourceHandle<StructuredBufferTag> deltaLights; ///< GPU に転送した点・スポット・方向光のバッファ。
    ResourceHandle<StructuredBufferTag> shapes; ///< GPU に転送した球・カプセル光源のバッファ。
    ResourceHandle<StructuredBufferTag> environmentTable; ///< 環境方向のサンプリング分布を GPU へ渡す表。
    ResourceHandle<TextureTag> environment; ///< 放射輝度を読む生の環境キューブのハンドル。
    RayEnvironmentDistribution environmentDistribution; ///< 環境の GPU 分布表を作った CPU レコードと内容版。
    uint64_t emitterRevision = 0; ///< GPU 側光源表に反映したパスシーンの内容版。
    bool sceneLighting = false; ///< シーンの光源と環境を反射評価に使えるか。
    bool diffuseIndirectEnabled = false; ///< このビューで共有の拡散間接光を参照できるか。
    bool constantEnvironmentKnown = false; ///< このビューの定数環境が検証済みか。
    math::Vector3 constantEnvironmentRadiance; ///< 検証済みの定数環境のシーン線形 RGB 放射輝度。
    bool cameraOriginProvenAir = false; ///< 透視カメラ原点が全検証済み中実形状の外側か。
    math::Vector3 provenAirOrigin; ///< 空気中にあることを証明した原点のワールド位置 [m]。
    RayReflectionReconstructionViewResources reconstruction; ///< このビューだけが保持する反射ガイドと時間再構成履歴。
};

} // namespace fbzz::renderer
