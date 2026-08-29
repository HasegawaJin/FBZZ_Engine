/// @file    FlipbookMotionVectors.hpp
/// @brief   フリップブックアトラスから モーションベクター アトラスを生成する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: motionVectorFlipbook はコマ間を速度場で warp してブレンドするため、
/// 見た目の滑らかさが MV アトラスの質でほぼ決まる。しかし MV は
/// 外部ツール (EmberGen 等) でしか作れず、手持ちのアトラスを持ち込むと
/// 「機能はあるのに使えない」状態になっていた。エンジン内で生成できるようにする。
///
/// アルゴリズムはブロックマッチング。Lucas-Kanade より実装が単純で、
/// フリップブック特有の「コマ間で大きく動く」ケースに強い。
#pragma once

#include <cstdint>
#include <string>

namespace fbzz::asset {

struct FlipbookMotionVectorSettings {
    int columns = 1;
    int rows = 1;
    // ブロックマッチングの窓とその探索半径 [px]。
    // 探索半径はコマ間の最大移動量に相当する。大きいほど速い動きを拾えるが
    // 計算量が半径の二乗で増える。
    int blockRadius = 4;
    int searchRadius = 8;
    // フロー場を均す回数。0 でブロック境界がそのまま出るため、通常 1 以上にする。
    int smoothIterations = 2;
    // 出力へ書き込む速度のスケール。1.0 で「探索半径 = ±1.0」に正規化される。
    float strength = 1.0f;
    // ループするアトラス (最終コマ → 先頭コマ) の動きも解析するか。
    bool loop = true;
    // 各行を独立したアニメーション列として扱う。
    // WHY: ParticleEmitter::spriteRandomRow は行末から同じ行の先頭へ戻るため、
    //      アトラス全体を直列解析すると行境界だけ別バリエーションへの誤った速度になる。
    bool rowSequences = false;
};

struct FlipbookMotionVectorResult {
    bool success = false;
    std::string outputPath;   // 生成した PNG の実パス
    std::string message;      // 失敗理由 / 成功時の要約 (UI へそのまま出せる文面)
    int frameCount = 0;
    float maxObservedFlow = 0.0f; // 実測の最大移動量 [px]。探索半径の妥当性判断に使う
};

// sourcePath のアトラスを解析し、RG に [-1,1] の速度を格納した PNG を
// "<source>_mv.png" として隣へ書き出す。
// 呼び出しはエディター操作 (数百ミリ秒〜数秒) を想定した同期処理。
[[nodiscard]] FlipbookMotionVectorResult GenerateFlipbookMotionVectors(
    const std::string& sourcePath, const FlipbookMotionVectorSettings& settings);

} // namespace fbzz::asset
