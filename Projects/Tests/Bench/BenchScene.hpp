/// @file    BenchScene.hpp
/// @brief   ビジュアル検証ベンチの 1 場面のインターフェース。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 自動テストで «数値が合っている» ことを確かめた上で、それでも目で見ないと
/// 判断できないもの (収束の様子、姿勢の崩れ方、法線の向き) をここに置く。
/// 合否はコードではなく人が決める ── だから Simulate と Draw しか持たない。
#pragma once

#include "Viewport2D.hpp"

namespace fbzz::bench {

class BenchScene {
public:
    virtual ~BenchScene() = default;

    /// 左のリストに出る名前。
    [[nodiscard]] virtual const char* Name() const = 0;
    /// 「何を見れば正しいと言えるか」を書く。ここが空だと誰も判定できない。
    [[nodiscard]] virtual const char* WhatToLookFor() const = 0;

    /// 初期状態へ戻す。場面を切り替えた直後にも呼ばれる。
    virtual void Reset() = 0;

    /// 時間を進める。一時停止中は呼ばれない。
    virtual void Simulate(float dt) = 0;

    /// この場面固有の設定 UI。
    virtual void DrawControls() = 0;

    /// ビューポートへの描画。
    virtual void Draw(Viewport2D& view) = 0;

    /// 既定のカメラ。場面を切り替えたときに適用する。
    virtual void ConfigureView(Viewport2D& view) = 0;
};

} // namespace fbzz::bench
