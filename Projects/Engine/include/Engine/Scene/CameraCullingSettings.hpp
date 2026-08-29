/// @file    CameraCullingSettings.hpp
/// @brief   CameraComponent のカリング設定を、値だけ抜き出した受け渡し用 POD。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY 独立したヘッダにするか: RenderSystem / RenderPassContext は Renderer 層寄りで
/// CameraComponent (Script / Layer 依存) を知らない。一方でこの値は
/// 「どのカメラで描くか」を決める呼び出し側と描画側の両方が要る。
/// SceneUtils.hpp (Window / ProjectSettings / physics::World を引く重いヘッダ) を
/// RenderSystem.hpp から include すると、Windows ヘッダのマクロ汚染までが
/// RenderSystem を include する全モジュールへ波及するため、値だけをここへ切り出す。
#pragma once

namespace fbzz::scene {

// レイヤー数。fbzz::Layer と同じ 0-31 の 32 本。
// WHY ここで持つか: このヘッダは Physics/Layer.hpp を引かずに済ませたい (依存を増やさない)。
//      値がずれると層ごとの距離が 1 つずつずれるため、静的検査は SceneUtils.cpp 側で行う。
inline constexpr int kCullLayerCount = 32;

struct CameraCullingSettings {
    // バウンディング球の視錐台テストを行うか。false ならフラスタムカリングをしない。
    bool  frustumCulling       = true;
    // CPU ソフトウェアオクルージョンカリングを行うか。
    //
    // WHY 既定を false にするか: 遮蔽者はバウンディング球の内接正方形として焼かれるため、
    //     床タイル・壁パネル・板ポリのような「球に対して実体が薄い」メッシュは、
    //     実際には何も無い空間まで「手前に不透明面がある」と主張してしまう。
    //     ゲームカメラは CameraComponent で明示的に有効化する前提とし、
    //     編集ビュー (Scene View / 各種プレビュー) のように設定を持たない呼び出しでは
    //     「見えているものが見える」ことを優先する。
    //     効きの薄さも根拠のひとつで、Terrain は GBuffer パスより後に描かれるため
    //     そもそも遮蔽者に登録されない (ワールド最大の遮蔽者が不参加のまま)。
    bool  occlusionCulling     = false;
    // 全バウンディング球へ加算するワールド単位の余白 [m]。負値は呼び出し側で 0 に丸める。
    float cullingBoundsPadding = 0.0f;

    // ---- 距離カリング ----
    // カメラから見えなくする距離 [m]。0 以下で無効 (farZ まで描く)。
    float maxDrawDistance = 0.0f;
    // レイヤーごとの距離上書き [m]。0 以下の要素は maxDrawDistance にフォールバックする。
    // WHY レイヤー粒度か: 「小物は 30m で消したいが建物は 500m まで残したい」が距離カリングの
    //      実用形で、オブジェクト個別に設定して回るのは現実的でない。Unity の
    //      Camera.layerCullDistances と同じ考え方。
    float layerCullDistances[kCullLayerCount] = {};
    // true = カメラ位置からの球距離、false = 視線方向の深度距離。
    // WHY 選べるようにするか: 球距離は視界を左右に振っても消える距離が変わらない代わりに、
    //      画面端のオブジェクトが中央より手前で消える。どちらが自然かはゲームの見せ方次第。
    bool  cullDistanceSpherical = true;

    // ---- 極小オブジェクトカリング ----
    // 画面高さに対するバウンディング球の相対高さがこの値未満なら描かない。0 以下で無効。
    // 単位は LODLevel::screenRelativeHeight と同じ (0.01 = 画面高さの 1%)。
    float smallObjectScreenHeight = 0.0f;
};

} // namespace fbzz::scene
