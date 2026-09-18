/// @file    RagdollProfile.hpp
/// @brief   骨の名前から、その骨の太さ・重さ・可動域・サーボ特性を決める規則
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// @note アセットでなく «名前の規則» で当てるのは、30 本の骨へ 1 本ずつ可動域を手で入れると骨格差し替えのたびに全部やり直しになるため。«*Knee* は片方向» のような骨格に依らない知識は規則として持つ方が移植が効く。
/// @note ロボットと人間を同じ構造で持つのは、違いが可動域の広さ・ドライブの硬さ・トルク上限の 3 つだけで仕組みは同じため。プリセット差し替えだけで «機械が軋む» と «ふにゃふにゃの人間» を出し分けられる。
#pragma once

#include <Physics/XPBDJoint.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

/// @brief サーボの «効き» を骨格に依らない量で書く。実際の [N·m] と [rad/(N·m)] は `RagdollRig::Build` が «その関節から先の質量» から埋める。
/// @note 絶対値で持たないのは、適正トルクが骨の長さと質量で桁違いになるため (Boss_01 の脚 7m・8 トンに人型想定の 14,000 N·m を当てたら股関節が力負けした)。倍率なら «どれだけ余裕があるか» が骨格を替えても読める。
struct RagdollServo {
    bool enabled = false;
    /// «その関節から先を、重力に対して真横へ伸ばした姿勢で支える» のに要るトルクへの倍率。1.0 でぎりぎり支え、上げるほど粘る。
    /// @note 真横の姿勢を基準にするのは、立ち姿勢はバインドポーズ次第で腕の長さがほぼ 0 になり得て、それを基準にすると必要トルクが 0 と出るため。姿勢に依らない «最悪の腕» を基準にすれば倍率がそのまま «どこまで傾けても耐えるか» になる。
    float torqueScale = 2.0f;
    /// トルク上限を出し切るところまで «たわむ» 角 [rad]。compliance をここから逆算する
    /// (α = 上限トルク / この角度 の逆数)。小さいほど硬い。
    float holdSag = 0.06f;
    /// 相対角速度を削る割合 [1/s]。ここだけは骨格に依らないのでそのまま持つ。
    float damping = 20.0f;
};

/// 骨 1 本ぶんの物理設定。
struct RagdollBoneSettings {
    /// カプセルの半径を «骨の長さに対する比» で持つ。
    /// @note 絶対値 [m] にしないのは密度と同じ理由。節の長さは骨格ごとに数倍違うため、絶対値だと «長い骨が針金のように細い» が起きる (Boss_01 の脛は 2.7m あり、人型想定の 0.14m は当たり判定 0.38m の 1/3 だった)。
    float radiusRatio = 0.16f;
    /// 半径の下限 [m]。極端に短い節がほぼ点になるのを防ぐ。
    float radiusMin = 0.03f;
    /// 密度 [kg/m^3]。質量はカプセルの体積から出す。
    /// @note 質量を直接持たないのは骨の長さが骨格ごとに違うため。質量を直に書くと骨格差し替えで «短い骨が重すぎる» が起きる。密度なら長さに追従する。
    float density = 900.0f;

    physics::XPBDJointLimits limits;
    RagdollServo             servo;
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

    /// この名前を含む骨から先は、ラグドールに含めない (枝ごと落とす)。
    /// @note Boss_01 は足指 3 本と踵が骨として生えており、そのまま拾うと脚 1 本につき剛体が 8 個増える。足指が動いても絵には出ないので、解く数だけが増える。
    std::vector<std::string> excludePatterns;
    /// 完全一致で枝ごと除外する骨名 (大文字小文字を無視)。短い略称を部分一致させないために使う。
    std::vector<std::string> excludeNames;
    [[nodiscard]] bool IsExcluded(std::string_view boneName) const;

    /// この名前を含む骨には剛体を作らない (**枝は辿る**)。
    /// @note excludePatterns と分けるのは、Root/Armature が原点付近の «入れ物» で最初の子まで数 m 離れることがあるため。枝ごと落とすと下の骨まで消えるが、そのまま拾うと原点へ伸びる棒が 1 本できる (Boss_01 は Root→Body が 4.5m)。剛体だけ作らないのが正しい扱い。
    std::vector<std::string> noBodyPatterns;
    [[nodiscard]] bool IsBodyless(std::string_view boneName) const;

    /// @param outMatched null でなければ «規則に当たったか» を返す。false は fallback。
    /// @note 当たらなくても settings は返るので、骨名が合っていないことは «なんとなく動くが可動域が全部球関節» としてしか現れない (Boss_01 で `Body`/`Hock` を実際に取りこぼしていた)。当たったかを別に返すのはこの取りこぼしを検出するため。
    [[nodiscard]] const RagdollBoneSettings& Resolve(std::string_view boneName,
                                                     bool* outMatched = nullptr) const;

    /// 重機・ロボット。狭い可動域、硬いドライブ、有限のサーボトルク。
    [[nodiscard]] static RagdollProfile Mech();
    /// 人型。広い可動域、柔らかいドライブ。
    [[nodiscard]] static RagdollProfile Humanoid();
};

} // namespace fbzz::scene
