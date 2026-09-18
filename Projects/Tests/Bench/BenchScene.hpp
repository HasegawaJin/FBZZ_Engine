/// @file    BenchScene.hpp
/// @brief   ビジュアル検証ベンチの 1 場面のインターフェース。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 自動テストで «数値が合っている» ことを確かめた上で、それでも目で見ないと
/// 判断できないもの (収束の様子、姿勢の崩れ方、法線の向き) をここに置く。
/// 合否はコードではなく人が決める。
///
/// ただし «人が見れば分かる» と «人が見続けていないと分からない» は別。後者を埋めるのが
/// DetectAnomalies で、絵からは読み取れない破綻 (NaN・貫通・法線の裏返り) を場面自身が申告する。
#pragma once

#include "AnomalyLog.hpp"
#include "Viewport2D.hpp"

namespace fbzz::bench {

/// @brief Simulate に渡す固定刻み [s]。画面モードと --measure で同じ値を使う。
inline constexpr float kBenchFixedStep = 1.0f / 60.0f;

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

    /// 描くための姿勢・デバッグ幾何を作り直す。1 フレームに 1 回だけ呼ばれる。
    /// @note Simulate は固定刻みを詰めるぶん 1 フレームに何度も回る。«最後の 1 回ぶんしか使われない» 出力をそこで作ると負荷が倍々になり、重い場面ほど刻み数が増えて更に重くなる (止まって見えるのはこの循環)。
    virtual void Present() {}

    /// この場面の不変条件を検査し、破れたものだけ log へ積む。毎フレーム呼ばれる。
    /// «設定次第で起こりうること» は書かない ── 常時点灯する警告は誰も読まなくなる。
    virtual void DetectAnomalies(AnomalyLog& log) = 0;

    /// この場面固有の設定 UI。
    virtual void DrawControls() = 0;

    /// ビューポートへの描画。
    virtual void Draw(Viewport2D& view) = 0;

    /// 既定のカメラ。場面を切り替えたときに適用する。
    virtual void ConfigureView(Viewport2D& view) = 0;
};

} // namespace fbzz::bench
