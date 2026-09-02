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

physics::XPBDJointDrive Servo(float compliance, float damping, float maxTorque)
{
    physics::XPBDJointDrive drive;
    drive.enabled    = true;
    drive.compliance = compliance;
    drive.damping    = damping;
    drive.maxTorque  = maxTorque;
    return drive;
}

} // namespace

const RagdollBoneSettings& RagdollProfile::Resolve(std::string_view boneName) const
{
    for (const Rule& rule : rules)
        if (ContainsIgnoreCase(boneName, rule.pattern)) return rule.settings;
    return fallback;
}

RagdollProfile RagdollProfile::Mech()
{
    RagdollProfile profile;

    // 機械の «素の関節»。狭く、硬く、そして有限のトルクしか出せない。
    profile.fallback.radius  = 0.16f;
    profile.fallback.density = 2200.0f;   // 中身の詰まった構造体に寄せる
    profile.fallback.limits  = Ball(18.0f, 5.0f);
    profile.fallback.drive   = Servo(2.0e-6f, 45.0f, 9000.0f);

    // 胴は関節ではなく «塊»。ほとんど動かない。
    profile.rules.push_back({ "spine",  profile.fallback });
    profile.rules.back().settings.radius = 0.34f;
    profile.rules.back().settings.limits = Ball(6.0f, 3.0f);
    profile.rules.back().settings.drive  = Servo(1.0e-6f, 60.0f, 26000.0f);

    // 膝は前へしか曲がらない。逆に折れないことが «機械に見える» の中心。
    for (const char* knee : { "knee", "shin", "calf" }) {
        Rule rule;
        rule.pattern           = knee;
        rule.settings.radius   = 0.14f;
        rule.settings.density  = 2400.0f;
        rule.settings.limits   = Hinge(0.0f, 105.0f);
        rule.settings.drive    = Servo(2.0e-6f, 45.0f, 7000.0f);
        profile.rules.push_back(rule);
    }

    // 股関節。踏ん張る側なので出力を高くする。
    for (const char* thigh : { "thigh", "hip", "upperleg" }) {
        Rule rule;
        rule.pattern          = thigh;
        rule.settings.radius  = 0.20f;
        rule.settings.density = 2400.0f;
        rule.settings.limits  = Ball(35.0f, 8.0f);
        rule.settings.drive   = Servo(1.5e-6f, 50.0f, 14000.0f);
        profile.rules.push_back(rule);
    }

    // 足首。可動域が狭く、ここが先に力負けすると «踏ん張り切れずに崩れる» が出る。
    for (const char* foot : { "foot", "ankle", "toe" }) {
        Rule rule;
        rule.pattern          = foot;
        rule.settings.radius  = 0.12f;
        rule.settings.density = 2400.0f;
        rule.settings.limits  = Ball(22.0f, 6.0f);
        rule.settings.drive   = Servo(3.0e-6f, 40.0f, 4500.0f);
        profile.rules.push_back(rule);
    }

    return profile;
}

RagdollProfile RagdollProfile::Humanoid()
{
    RagdollProfile profile;

    profile.fallback.radius  = 0.06f;
    profile.fallback.density = 1000.0f;
    profile.fallback.limits  = Ball(45.0f, 25.0f);
    profile.fallback.drive   = Servo(4.0e-4f, 8.0f, 120.0f);

    profile.rules.push_back({ "spine", profile.fallback });
    profile.rules.back().settings.radius = 0.13f;
    profile.rules.back().settings.limits = Ball(25.0f, 20.0f);
    profile.rules.back().settings.drive  = Servo(2.0e-4f, 10.0f, 260.0f);

    for (const char* knee : { "knee", "shin", "calf", "elbow", "forearm" }) {
        Rule rule;
        rule.pattern         = knee;
        rule.settings.radius = 0.05f;
        rule.settings.limits = Hinge(0.0f, 140.0f, 8.0f);
        rule.settings.drive  = Servo(5.0e-4f, 8.0f, 90.0f);
        profile.rules.push_back(rule);
    }

    for (const char* ball : { "thigh", "upperleg", "shoulder", "upperarm", "arm" }) {
        Rule rule;
        rule.pattern         = ball;
        rule.settings.radius = 0.07f;
        rule.settings.limits = Ball(70.0f, 40.0f);
        rule.settings.drive  = Servo(4.0e-4f, 8.0f, 150.0f);
        profile.rules.push_back(rule);
    }

    Rule head;
    head.pattern         = "head";
    head.settings.radius = 0.10f;
    head.settings.limits = Ball(40.0f, 45.0f);
    head.settings.drive  = Servo(6.0e-4f, 6.0f, 40.0f);
    profile.rules.push_back(head);

    return profile;
}

} // namespace fbzz::scene
