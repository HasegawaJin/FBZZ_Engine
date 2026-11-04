/// @file    FlipbookMotionVectors.hpp
/// @brief   フリップブックアトラスから モーションベクター アトラスを生成する。
/// @author  Hasegawa Jin
/// @date    2026-08-12

/// @note MV アトラスの質がコマ間 warp の滑らかさを決めるが、外部ツール (EmberGen 等) でしか
/// @note 作れず「機能はあるが使えない」状態だったため、エンジン内でも生成できるようにした。
/// @note アルゴリズムはブロックマッチング (Lucas-Kanade より単純、コマ間の大きな動きに強い)。
/// @note 保存値の規約は FlipbookMotionVectorEncoding.hpp に従う。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector2.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::asset {

struct FlipbookMotionVectorSettings {
    int columns = 1;
    int rows = 1;
    /// @note ブロックマッチングの窓とその探索半径 [px]。
    /// @note 探索半径はコマ間の最大移動量に相当する。大きいほど速い動きを拾えるが
    /// @note 計算量が半径の二乗で増える。
    int blockRadius = 4;
    int searchRadius = 8;
    /// @note フロー場を均す回数。0 でブロック境界がそのまま出るため、通常 1 以上にする。
    int smoothIterations = 2;
    /// @note ループするアトラス (最終コマ → 先頭コマ) の動きも解析するか。
    bool loop = true;
    /// @note 各行を独立したアニメーション列として扱う。
    /// @note spriteRandomRow は行末から同じ行の先頭へ戻るため、アトラス全体を直列解析すると
    /// @note 行境界だけ別バリエーションへの誤った速度になる。
    bool rowSequences = false;
};

/// @note ファイルを扱わない解析結果。テストと GenerateFlipbookMotionVectors が共有する。
struct FlipbookMotionAnalysis {
    bool success = false;
    std::string message;
    /// @note Atlas と同じ寸法。各画素に «そのコマ → 次のコマ» の移動量を Atlas UV で持つ。
    std::vector<math::Vector2> displacementUv;
    /// @note 規約の S。マテリアルの motionVectorStrength にそのまま入れる値。
    float recommendedStrength = 0.0f;
    float maxObservedFlow = 0.0f; ///< @note 実測の最大移動量 [px]。探索半径の妥当性判断に使う
    int frameCount = 0;
    int searchRadius = 0;
};

struct FlipbookMotionVectorResult {
    bool success = false;
    std::string outputPath;   ///< @note 生成した PNG の実パス
    std::string message;      ///< @note 失敗理由 / 成功時の要約 (UI へそのまま出せる文面)
    int frameCount = 0;
    float maxObservedFlow = 0.0f; ///< @note 実測の最大移動量 [px]。探索半径の妥当性判断に使う
    /// @note マテリアルの motionVectorStrength に設定する値。これ以外の値では warp 量が合わない。
    float recommendedStrength = 0.0f;
};

/// @note rgba は width x height の RGBA float を詰めて並べたもの。
[[nodiscard]] FBZZ_ENGINE_API FlipbookMotionAnalysis AnalyzeFlipbookMotion(
    std::span<const float> rgba, std::uint32_t width, std::uint32_t height,
    const FlipbookMotionVectorSettings& settings);

/// @note sourcePath のアトラスを解析し、`"<source>_mv.png"` として隣へ書き出す。
/// @note 呼び出しはエディター操作 (数百ミリ秒〜数秒) を想定した同期処理。
[[nodiscard]] FBZZ_ENGINE_API FlipbookMotionVectorResult GenerateFlipbookMotionVectors(
    const std::string& sourcePath, const FlipbookMotionVectorSettings& settings);

} /// @note namespace fbzz::asset
