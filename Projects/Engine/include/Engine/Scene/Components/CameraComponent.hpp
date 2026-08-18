// FBZZ Engine
// CameraComponent.hpp | fbzz::scene
// カメラパラメータを持つコンポーネント
// 位置と回転は GameObject の Transform を正とし、この型は投影設定を持つ。
// RenderSystem が Camera へ変換して描画に使う。
#pragma once
#include <Physics/Layer.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

struct CameraComponent {
    float fovY    = 60.0f;
    // WHY: RenderSystem 呼び出し元の viewport aspect と CameraComponent の投影設定を同期し、
    //      Script の座標変換 API でも同じ投影行列を再現できるように Scene 側へ保持する。
    float aspectRatio = 16.0f / 9.0f;
    float nearZ   = 0.1f;
    float farZ    = 1000.0f;
    bool  isMain  = true;
    bool  enabled = true;
    fbzz::LayerMask cullingMask = fbzz::Layer::Everything;

    // ---- カリング設定 ----
    // WHY ここに置くか: フラスタム / オクルージョンの可否は「このカメラで何を描くか」の
    //      一部で、cullingMask と同じ粒度で切り替えたい。RenderSettings (プロジェクト共通)
    //      に置くと、ゲームカメラとサブカメラで別々の判断ができなくなる。

    // false にするとバウンディング球の視錐台テストを行わず、全オブジェクトを提出する。
    // WHY 切れるようにするか: 「消えたのはカリングのせいか、それとも別の理由か」を
    //      1 クリックで切り分けられないと、バウンディング球の不備とマテリアル欠落や
    //      lodVisible の誤りを区別できない。
    bool  frustumCulling = true;

    // CPU ソフトウェアオクルージョンカリング (OcclusionCuller) の有無。
    // NOTE: 遮蔽者はメッシュ実体ではなくバウンディング球の近似なので、
    //       保守的側 (落としすぎない側) に倒してある。数値は Analysis パネルで確認できる。
    bool  occlusionCulling = true;

    // 全バウンディング球へ加算するワールド単位の余白 [m]。
    // WHY: スキンドメッシュの球はバインドポーズから作るため、大きく腕を振る / 武器を
    //      伸ばすモーションでは実際のシルエットが球からはみ出し、画面端で消える。
    //      厳密なポーズ bounds を毎フレーム作るのは高価なので、余白で吸収する。
    float cullingBoundsPadding = 0.0f;

    // 描画距離の上限 [m]。0 以下で無効 (farZ まで描く)。
    // WHY farZ と別に持つか: farZ を縮めると深度バッファの分解能配分そのものが変わり、
    //      Z ファイティングや影の品質まで動く。「遠くの物を描かない」だけを、
    //      投影行列に触れずに済ませたい。
    float maxDrawDistance = 0.0f;

    // レイヤーごとの描画距離上書き [m]。0 以下の要素は maxDrawDistance を使う。
    float layerCullDistances[kCullLayerCount] = {};

    // 距離の測り方。true = カメラからの球距離、false = 視線方向の深度距離。
    bool  cullDistanceSpherical = true;

    // バウンディング球の画面高さ比がこの値未満なら描かない。0 以下で無効。
    // 単位は LODLevel::screenRelativeHeight と同じ (0.01 = 画面高さの 1%)。
    // WHY LODGroup と別に要るか: LODGroup は「設定した個々のオブジェクト」にしか効かない。
    //      こちらはシーン全体へ一律に効く保険で、LOD を組んでいないアセットが
    //      遠景で 1 ピクセル未満の DrawCall を出し続けるのを止める。
    float smallObjectScreenHeight = 0.0f;

    const char* GetTypeName() const { return "Camera"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("isMain", isMain);
        r.Field("fovY", fovY);
        r.Field("aspectRatio", aspectRatio);
        r.Field("nearZ", nearZ);
        r.Field("farZ", farZ);
        r.Field("cullingMask", reinterpret_cast<int&>(cullingMask));
        r.Field("frustumCulling", frustumCulling);
        r.Field("occlusionCulling", occlusionCulling);
        r.Field("cullingBoundsPadding", cullingBoundsPadding);
        r.Field("maxDrawDistance", maxDrawDistance);
        r.Field("cullDistanceSpherical", cullDistanceSpherical);
        r.Field("smallObjectScreenHeight", smallObjectScreenHeight);
        // layerCullDistances は固定長配列で IReflector に対応する Field が無い。
        // 永続化は SceneSerializer が配列として書き出す。
    }
};

} // namespace fbzz::scene
