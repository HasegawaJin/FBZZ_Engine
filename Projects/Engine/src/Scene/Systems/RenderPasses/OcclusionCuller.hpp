// FBZZ Engine
// OcclusionCuller.hpp | fbzz::scene
// CPU ソフトウェアオクルージョンカリング
//
// 概要:
//   128×72 のリニア深度バッファを CPU 上に持ち、ワールド空間のバウンディング球を
//   スクリーン空間に投影して可視判定とオクルーダー登録を行う。
//
// アルゴリズム:
//   1. Reset() でフレーム開始時に深度バッファをクリア (初期値 1.0 = 無限遠)
//   2. オブジェクトを「カメラから近い順」に並べた上で TestAndRaster() を呼ぶ
//      - 球の最近点 (nearLinDepth) が深度バッファ全域より奥にある → 完全隠蔽 → false
//      - 可視なら球の前面深度を深度バッファに書き込み → 以降の球がここで遮られる
//   3. false が返ったオブジェクトは DrawCall を省略できる
//
// WHY: GPU Occlusion Query は結果が 1 フレーム遅延するため、高速移動シーンで
//      誤カリングが起きやすい。CPU SW 方式は即時結果かつ実装が単純。
//      代わりに精度は保守的 (false negative あり = 隠れていても描画することがある)。
//      false positive (見えているのにカリング) は発生しない。
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class OcclusionCuller {
public:
    // 深度バッファ解像度: 16:9 を意識した小サイズ。
    // 大きいほど精度は上がるが CPU キャッシュ圧力と計算コストが増える。
    static constexpr int kWidth  = 128;
    static constexpr int kHeight = 72;

    OcclusionCuller();

    // フレーム開始時に呼ぶ。カメラ行列を記録し、深度バッファを 1.0 で初期化する。
    void Reset(const renderer::Camera& cam);

    // ワールド空間のバウンディング球を可視判定する。
    //   true  → 可視 (描画すべき)。球を深度バッファにオクルーダーとして登録する。
    //   false → 完全隠蔽確定 (描画スキップ可能)。
    // オブジェクトは「カメラから近い順」に処理しないと効果が薄い。
    bool TestAndRaster(const math::Vector3& worldCenter, float worldRadius);

private:
    // リニア深度 [0=nearZ, 1=farZ]。小さい値ほどカメラに近い。
    float m_depth[kWidth * kHeight];

    math::Matrix4 m_view;
    math::Matrix4 m_proj;
    float         m_nearZ = 0.1f;
    float         m_farZ  = 1000.0f;

    // ワールド空間の点をスクリーン座標とリニア深度に変換する。
    // スクリーン外・カメラ後方は false を返す。
    bool ProjectPoint(const math::Vector3& worldPos,
                      float& outSx, float& outSy,
                      float& outLinearDepth) const;
};

} // namespace fbzz::scene
