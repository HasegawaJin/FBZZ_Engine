/// @file    RagdollProfile.cpp
/// @brief   骨名から物理設定を引く規則と、ロボット / 人型のプリセット
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Engine/Scene/Ragdoll/RagdollProfile.hpp>

#include <Math/MathUtils.hpp>

#include <algorithm>
#include <cctype>

namespace fbzz::scene {

namespace {

constexpr float kDegreesToRadians = math::PI / 180.0f;
constexpr float Degrees(float value) { return value * kDegreesToRadians; }

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle)
{
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [&lower](char a, char b) { return lower(a) == lower(b); });
    return it != haystack.end();
}

/// 蝶番。1 軸だけ、しかも片方向にしか曲がらない関節 (膝・肘・指)。
physics::XPBDJointLimits Hinge(float minDegrees, float maxDegrees, float slopDegrees = 3.0f)
{
    physics::XPBDJointLimits limits;
    limits.enabled   = true;
    limits.twistMin  = -Degrees(slopDegrees);
    limits.twistMax  = Degrees(slopDegrees);
    limits.swingMinY = -Degrees(slopDegrees);
    limits.swingMaxY = Degrees(slopDegrees);
    limits.swingMinZ = Degrees(minDegrees);
    limits.swingMaxZ = Degrees(maxDegrees);
    return limits;
}

/// 球関節。2 方向へ曲がり、ねじれる (肩・股・首・胴)。
physics::XPBDJointLimits Ball(float swingDegrees, float twistDegrees)
{
    physics::XPBDJointLimits limits;
    limits.enabled   = true;
    limits.twistMin  = -Degrees(twistDegrees);
    limits.twistMax  = Degrees(twistDegrees);
    limits.swingMinY = -Degrees(swingDegrees);
    limits.swingMaxY = Degrees(swingDegrees);
    limits.swingMinZ = -Degrees(swingDegrees);
    limits.swingMaxZ = Degrees(swingDegrees);
    return limits;
}

/// @param torqueScale 自重を真横で支えるのに要るトルクへの倍率。1.0 でぎりぎり。
/// @param sagDegrees  その姿勢で支えているときの定常たわみ [度]。
/// @param damping     相対角速度を削る割合 [1/s]。
RagdollServo Servo(float torqueScale, float sagDegrees, float damping)
{
    RagdollServo servo;
    servo.enabled     = true;
    servo.torqueScale = torqueScale;
    servo.holdSag     = Degrees(sagDegrees);
    servo.damping     = damping;
    return servo;
}

} // namespace

bool RagdollProfile::IsExcluded(std::string_view boneName) const
{
    for (const std::string& pattern : excludePatterns)
        if (ContainsIgnoreCase(boneName, pattern)) return true;
    for (const std::string& name : excludeNames)
        if (boneName.size() == name.size() && ContainsIgnoreCase(boneName, name)) return true;
    return false;
}

bool RagdollProfile::IsBodyless(std::string_view boneName) const
{
    for (const std::string& pattern : noBodyPatterns)
        if (ContainsIgnoreCase(boneName, pattern)) return true;
    return false;
}

const RagdollBoneSettings& RagdollProfile::Resolve(std::string_view boneName,
                                                   bool* outMatched) const
{
    for (const Rule& rule : rules) {
        if (!ContainsIgnoreCase(boneName, rule.pattern)) continue;
        if (outMatched) *outMatched = true;
        return rule.settings;
    }
    if (outMatched) *outMatched = false;
    return fallback;
}

RagdollProfile RagdollProfile::Mech()
{
    RagdollProfile profile;

    /// @note 原点に置かれた «入れ物»。剛体は作らず、枝だけ辿る。
    profile.noBodyPatterns = { "root", "armature", "motion", "scene" };
    /// @note 足指と踵。ラグドールで動いても絵に出ないのに、脚 1 本あたり剛体が 8 個増える。
    profile.excludePatterns = { "toe", "heel", "finger", "thumb" };

    /// @note 機械の «素の関節»。狭く、硬く、そして有限のトルクしか出せない。装甲板とフレームの
    ///       中空構造のため密度は 400: 中身の詰まった鋼材 (7800) で見積もると Boss_01 が 70 トンを
    ///       超え、瓦礫を «弾く» が «消し飛ばす» になる。質量は倒れる速さには効かない (重力も同じ
    ///       だけ増えるため) が、接触で相手へ返す力積には効くので、見た目の大きさに対して妥当な桁へ置く。
    profile.fallback.radiusRatio = 0.16f;
    profile.fallback.radiusMin   = 0.04f;
    profile.fallback.density     = 400.0f;
    profile.fallback.limits      = Ball(18.0f, 5.0f);
    profile.fallback.servo       = Servo(2.0f, 3.0f, 45.0f);

    /// @note 胴は関節ではなく «塊»。ほとんど動かず、桁違いに粘る。人型の骨格は Spine、
    ///       四足やロボットは Body と名付けるため両方を列挙する: Boss_01 は Body で、spine だけを
    ///       見ていた頃は胴が fallback の細い棒になり、«塊» のつもりで書いた半径もトルクも
    ///       1 度も使われていなかった。
    for (const char* torso : { "spine", "body", "torso", "chest", "pelvis" }) {
        Rule rule;
        rule.pattern              = torso;
        rule.settings.radiusRatio = 0.70f;
        rule.settings.radiusMin   = 0.10f;
        rule.settings.density     = 400.0f;
        rule.settings.limits      = Ball(6.0f, 3.0f);
        rule.settings.servo       = Servo(6.0f, 0.5f, 60.0f);
        profile.rules.push_back(rule);
    }

    /// @note 胴から生えた突起 (砲身・タレット)。関節としてはほぼ動かない。
    for (const char* mount : { "core", "muzzle", "barrel", "turret" }) {
        Rule rule;
        rule.pattern              = mount;
        rule.settings.radiusRatio = 0.38f;
        rule.settings.radiusMin   = 0.06f;
        rule.settings.density     = 400.0f;
        rule.settings.limits      = Ball(4.0f, 2.0f);
        rule.settings.servo       = Servo(6.0f, 0.5f, 60.0f);
        profile.rules.push_back(rule);
    }

    /// @note 膝は前へしか曲がらない。逆に折れないことが «機械に見える» の中心。関節フレームの
    ///       Z 軸は骨の向きから作るため «前» がどちら側かは FBX のボーン軸で決まり、符号が逆だと
    ///       «曲がるべき方向に 0°» になって脚が棒のまま倒れる。Overlays > Ragdoll で錐を見て、
    ///       開いている側が曲がるべき側か確かめ、逆なら min/max を入れ替えること。
    for (const char* knee : { "knee", "shin", "calf" }) {
        Rule rule;
        rule.pattern              = knee;
        rule.settings.radiusRatio = 0.14f;
        rule.settings.radiusMin   = 0.04f;
        rule.settings.density     = 450.0f;
        rule.settings.limits      = Hinge(0.0f, 105.0f);
        rule.settings.servo       = Servo(2.5f, 2.0f, 45.0f);
        profile.rules.push_back(rule);
    }

    /// @note 飛節 (四足の後肢で踵にあたる関節)。膝と同じ蝶番だが**逆向きに**曲がる。
    ///       Hock → Foot が接地する節。ここが先に力負けすると «踏ん張り切れずに崩れる» が出る。
    for (const char* hock : { "hock", "hough" }) {
        Rule rule;
        rule.pattern              = hock;
        rule.settings.radiusRatio = 0.16f;
        rule.settings.radiusMin   = 0.04f;
        rule.settings.density     = 450.0f;
        rule.settings.limits      = Hinge(-105.0f, 0.0f);
        rule.settings.servo       = Servo(1.8f, 3.0f, 40.0f);
        profile.rules.push_back(rule);
    }

    /// @note 股関節。踏ん張る側なので出力を高くする。Yaw は脚の付け根のヨー軸で、
    ///       節が短いぶん «付け根の球» になる。
    for (const char* thigh : { "thigh", "hip", "upperleg", "yaw" }) {
        Rule rule;
        rule.pattern              = thigh;
        rule.settings.radiusRatio = 0.25f;
        rule.settings.radiusMin   = 0.05f;
        rule.settings.density     = 450.0f;
        rule.settings.limits      = Ball(35.0f, 8.0f);
        rule.settings.servo       = Servo(3.0f, 2.0f, 50.0f);
        profile.rules.push_back(rule);
    }

    /// @note 足首。可動域が狭い。
    for (const char* foot : { "foot", "ankle" }) {
        Rule rule;
        rule.pattern              = foot;
        rule.settings.radiusRatio = 0.35f;
        rule.settings.radiusMin   = 0.05f;
        rule.settings.density     = 450.0f;
        rule.settings.limits      = Ball(22.0f, 6.0f);
        rule.settings.servo       = Servo(1.5f, 3.0f, 40.0f);
        profile.rules.push_back(rule);
    }

    return profile;
}

RagdollProfile RagdollProfile::Humanoid()
{
    RagdollProfile profile;

    profile.noBodyPatterns  = { "root", "armature", "motion", "scene" };
    /// @note Player の短縮名の指と装備・アンテナは親の姿勢へ追従させ、独立した剛体を作らない。
    profile.excludePatterns = { "finger", "thumb", "index", "middle", "pinky", "toe" };
    profile.excludeNames = {
        "Mount_Back", "Ant_A", "Ant_B", "Grip_L", "Grip_R",
        "F1A_L", "F2A_L", "F3A_L", "ThA_L", "F1B_L", "F2B_L", "F3B_L", "ThB_L",
        "F1A_R", "F2A_R", "F3A_R", "ThA_R", "F1B_R", "F2B_R", "F3B_R", "ThB_R"
    };

    /// @note 人体の平均密度は水と同じくらい。
    profile.fallback.radiusRatio = 0.18f;
    profile.fallback.radiusMin   = 0.02f;
    profile.fallback.density     = 1000.0f;
    profile.fallback.limits      = Ball(45.0f, 25.0f);
    profile.fallback.servo       = Servo(1.2f, 10.0f, 8.0f);

    /// @note Mech と同じ理由で名前を並べる。人型でも腰は Hips / Pelvis と名付けられる方が多い。
    for (const char* torso : { "spine", "hips", "pelvis", "chest", "torso" }) {
        Rule rule;
        rule.pattern              = torso;
        rule.settings.radiusRatio = 0.50f;
        rule.settings.radiusMin   = 0.06f;
        rule.settings.density     = 1000.0f;
        rule.settings.limits      = Ball(25.0f, 20.0f);
        rule.settings.servo       = Servo(2.0f, 8.0f, 10.0f);
        profile.rules.push_back(rule);
    }

    for (const char* knee : { "knee", "shin", "calf", "elbow", "forearm" }) {
        Rule rule;
        rule.pattern              = knee;
        rule.settings.radiusRatio = 0.18f;
        rule.settings.radiusMin   = 0.02f;
        rule.settings.limits      = Hinge(0.0f, 140.0f, 8.0f);
        rule.settings.servo       = Servo(1.0f, 12.0f, 8.0f);
        profile.rules.push_back(rule);
    }

    for (const char* ball : { "thigh", "upperleg", "shoulder", "upperarm", "arm" }) {
        Rule rule;
        rule.pattern              = ball;
        rule.settings.radiusRatio = 0.20f;
        rule.settings.radiusMin   = 0.03f;
        rule.settings.limits      = Ball(70.0f, 40.0f);
        rule.settings.servo       = Servo(1.2f, 10.0f, 8.0f);
        profile.rules.push_back(rule);
    }

    for (const char* wrist : { "hand", "wrist" }) {
        Rule rule;
        rule.pattern = wrist;
        rule.settings.radiusRatio = 0.28f;
        rule.settings.radiusMin = 0.02f;
        rule.settings.density = 1000.0f;
        rule.settings.limits = Ball(35.0f, 20.0f);
        rule.settings.servo = Servo(1.0f, 12.0f, 8.0f);
        profile.rules.push_back(rule);
    }

    Rule head;
    head.pattern              = "head";
    head.settings.radiusRatio = 0.50f;
    head.settings.radiusMin   = 0.06f;
    head.settings.limits      = Ball(40.0f, 45.0f);
    head.settings.servo       = Servo(1.2f, 12.0f, 6.0f);
    profile.rules.push_back(head);

    return profile;
}

} // namespace fbzz::scene
