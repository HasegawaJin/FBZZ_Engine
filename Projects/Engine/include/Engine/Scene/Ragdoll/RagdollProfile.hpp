/// @file    RagdollProfile.hpp
/// @brief   骨の名前から、その骨の太さ・重さ・可動域・サーボ特性を決める規則
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// WHY アセットではなく «名前の規則» で当てるか:
///   30 本の骨へ 1 本ずつ可動域を手で入れるのは、骨格を差し替えるたびに全部やり直しになる。
///   «*Knee* は片方向にしか曲がらない» は骨格が変わっても変わらない知識なので、規則として
///   持つ方が移植が効く。個別に詰めたくなったら、規則を細かく足していけばよい。
///
/// WHY ロボットと人間を同じ構造で持つか:
///   違いは可動域の広さ・ドライブの硬さ・トルク上限の 3 つだけで、仕組みは同じ。
///   プリセットを差し替えるだけで «機械が軋む» と «ふにゃふにゃの人間» を出し分けられる
///   ことが、汎用ラグドールとして持つ意味そのものになる。
#pragma once

#include <Physics/XPBDJoint.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

/// 骨 1 本ぶんの物理設定。
struct RagdollBoneSettings {
    /// カプセルの半径 [m]。骨の «太さ»。
    float radius = 0.08f;
    /// 密度 [kg/m^3]。質量はカプセルの体積から出す。
    ///
    /// WHY 質量を直接持たないか: 骨の長さは骨格ごとに違う。質量を直に書くと、骨格を
    ///     差し替えたときに «短い骨が重すぎる» が起きる。密度なら長さに追従する。
    float density = 900.0f;

    physics::XPBDJointLimits limits;
    physics::XPBDJointDrive  drive;
};

/// 骨名のパターンで設定を割り当てる。先に書いた規則が勝つ。
struct RagdollProfile {
    struct Rule {
        /// 骨名に含まれていれば一致 (大文字小文字を無視)。
        std::string         pattern;
        RagdollBoneSettings settings;
    };

    /// どの規則にも当たらなかった骨に使う。
    RagdollBoneSettings fallback;
    std::vector<Rule>   rules;

    [[nodiscard]] const RagdollBoneSettings& Resolve(std::string_view boneName) const;

    /// 重機・ロボット。狭い可動域、硬いドライブ、有限のサーボトルク。
    [[nodiscard]] static RagdollProfile Mech();
    /// 人型。広い可動域、柔らかいドライブ。
    [[nodiscard]] static RagdollProfile Humanoid();
};

} // namespace fbzz::scene
