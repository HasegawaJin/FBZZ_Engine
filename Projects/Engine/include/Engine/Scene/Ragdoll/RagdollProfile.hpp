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

/// サーボの «効き» を骨格に依らない量で書く。実際の [N·m] と [rad/(N·m)] は
/// `RagdollRig::Build` が «その関節から先の質量» から埋める。
///
/// WHY 絶対値で持たないか: 適正なトルクは骨の長さと質量で決まり、骨格ごとに桁が違う。
///     Boss_01 (脚だけで 7m、総質量 8 トン) に人型の想定で書いた 14,000 N·m を当てたら、
///     股関節が必要量の 1/3 しか出せず脚が畳まれた。倍率で持てば «どれだけ余裕があるか»
///     という読める量になり、骨格を差し替えても意味が保たれる ─ 密度で質量を持つのと同じ理屈。
struct RagdollServo {
    bool enabled = false;
    /// «その関節から先を、重力に対して真横へ伸ばした姿勢で支える» のに要るトルクへの倍率。
    /// 1.0 でぎりぎり支え、少し押せば負ける。上げるほど粘る。
    ///
    /// WHY 真横の姿勢を基準にするか: 立ち姿勢はバインドポーズ次第で腕の長さがほぼ 0 に
    ///     なることがあり、それを基準にすると必要トルクが 0 と出る。姿勢に依らず決まる
    ///     «最悪の腕» を基準にすれば、倍率がそのまま «どこまで傾けても耐えるか» になる。
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
    ///
    /// WHY 絶対値 [m] にしないか: 密度と同じ理由。節の長さは骨格ごとに数倍違うので、
    ///     絶対値だと «長い骨が針金のように細い» が起きる。Boss_01 の脛は 2.7m あり、
    ///     人型の想定で置いた 0.14m は当たり判定 (0.38m) の 3 分の 1 だった。
    float radiusRatio = 0.16f;
    /// 半径の下限 [m]。極端に短い節がほぼ点になるのを防ぐ。
    float radiusMin = 0.03f;
    /// 密度 [kg/m^3]。質量はカプセルの体積から出す。
    ///
    /// WHY 質量を直接持たないか: 骨の長さは骨格ごとに違う。質量を直に書くと、骨格を
    ///     差し替えたときに «短い骨が重すぎる» が起きる。密度なら長さに追従する。
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
    ///
    /// WHY 要るか: Boss_01 は足指 3 本と踵が骨として生えていて、そのまま拾うと
    ///     脚 1 本につき剛体が 8 個増える。ラグドールで足指が動いても絵には出ないので、
    ///     解く数だけが増えることになる。
    std::vector<std::string> excludePatterns;
    /// 完全一致で枝ごと除外する骨名 (大文字小文字を無視)。短い略称を部分一致させないために使う。
    std::vector<std::string> excludeNames;
    [[nodiscard]] bool IsExcluded(std::string_view boneName) const;

    /// この名前を含む骨には剛体を作らない (**枝は辿る**)。
    ///
    /// WHY excludePatterns と分けるか: Root や Armature は原点に置かれた «入れ物» で、
    ///     そこから最初の子まで数 m 離れていることがある。枝ごと落とすと下の骨まで
    ///     消えてしまうが、そのまま拾うと胴から原点へ伸びる棒が 1 本できる
    ///     (Boss_01 は Root → Body が 4.5m)。剛体だけ作らないのが正しい扱い。
    std::vector<std::string> noBodyPatterns;
    [[nodiscard]] bool IsBodyless(std::string_view boneName) const;

    /// @param outMatched null でなければ «規則に当たったか» を返す。false は fallback。
    ///
    /// WHY 当たったかを返すか: 当たらなくても settings は返るので、骨名が合っていない
    ///     ことが «なんとなく動くが可動域が全部球関節» という形でしか現れない。
    ///     Boss_01 で実際に `Body` と `Hock` を取りこぼしていたのがこれ。
    [[nodiscard]] const RagdollBoneSettings& Resolve(std::string_view boneName,
                                                     bool* outMatched = nullptr) const;

    /// 重機・ロボット。狭い可動域、硬いドライブ、有限のサーボトルク。
    [[nodiscard]] static RagdollProfile Mech();
    /// 人型。広い可動域、柔らかいドライブ。
    [[nodiscard]] static RagdollProfile Humanoid();
};

} // namespace fbzz::scene
