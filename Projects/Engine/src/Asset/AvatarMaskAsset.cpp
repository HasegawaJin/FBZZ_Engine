// FBZZ Engine
// AvatarMaskAsset.cpp | fbzz::asset
// .mask アセットの TOML 入出力とボーン別ウェイト評価
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace fbzz::asset {

namespace {

std::string LowerCopy(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// パスの階層数 ("Hips/Spine" → 1)。エントリの具体度比較と blendDepth 計算に使う。
int PathDepth(std::string_view path)
{
    return static_cast<int>(std::count(path.begin(), path.end(), '/'));
}

// entry.bonePath が bonePath / boneName に一致するか判定し、一致なら
// 「エントリのボーンから何階層下か」を outDepth に返す。
// WHY: blendDepth による立ち上げは、この深さ差からしか計算できない。
bool MatchEntry(const AvatarMaskEntry& entry,
                std::string_view bonePath,
                std::string_view boneName,
                int& outDepth)
{
    if (entry.bonePath.empty()) return false;

    // 完全一致 (パス指定・名前指定のどちらでも受ける)。
    if (entry.bonePath == bonePath || entry.bonePath == boneName) {
        outDepth = 0;
        return true;
    }
    if (!entry.includeChildren) return false;

    // パス指定の祖先一致: "Hips/Spine" は "Hips/Spine/Spine1" にヒットする。
    if (bonePath.size() > entry.bonePath.size() &&
        bonePath.compare(0, entry.bonePath.size(), entry.bonePath) == 0 &&
        bonePath[entry.bonePath.size()] == '/') {
        outDepth = PathDepth(bonePath) - PathDepth(entry.bonePath);
        return true;
    }

    // 名前指定の祖先一致: "Spine" は ".../Spine/Spine1" にヒットする。
    // WHY: マスクを手書きするときパスを全部書かせたくない。ボーン名だけで
    //      その配下を指定できると、スケルトンが違っても同じマスクが使い回せる。
    if (entry.bonePath.find('/') == std::string::npos && !bonePath.empty()) {
        const std::string needle = "/" + entry.bonePath + "/";
        const size_t pos = bonePath.find(needle);
        if (pos != std::string_view::npos) {
            const int entryDepth =
                PathDepth(bonePath.substr(0, pos + needle.size() - 1));
            outDepth = PathDepth(bonePath) - entryDepth;
            return true;
        }
        // 先頭がそのボーンの場合 ("Spine/Spine1")。
        if (bonePath.size() > entry.bonePath.size() &&
            bonePath.compare(0, entry.bonePath.size(), entry.bonePath) == 0 &&
            bonePath[entry.bonePath.size()] == '/') {
            outDepth = PathDepth(bonePath);
            return true;
        }
    }
    return false;
}

// blendDepth に沿って深さ depth のウェイトを求める。
// depth = 0 で weight/(blendDepth+1)、depth >= blendDepth で weight に到達する。
float RampedWeight(const AvatarMaskEntry& entry, int depth)
{
    if (entry.blendDepth <= 0) return entry.weight;
    const float t = static_cast<float>(std::min(depth + 1, entry.blendDepth + 1)) /
                    static_cast<float>(entry.blendDepth + 1);
    return entry.weight * t;
}

// 体パーツごとのボーン名トークン。小文字部分一致で判定する。
const std::array<std::vector<std::string>, static_cast<size_t>(HumanoidBodyPart::Count)>&
HumanoidPatternTable()
{
    static const std::array<std::vector<std::string>, static_cast<size_t>(HumanoidBodyPart::Count)> table = {
        std::vector<std::string>{ "root", "reference", "armature" },
        std::vector<std::string>{ "hips", "pelvis", "spine", "chest", "torso", "waist", "abdomen" },
        std::vector<std::string>{ "neck", "head", "jaw", "eye" },
        std::vector<std::string>{ "leftshoulder", "leftarm", "leftforearm", "leftupperarm", "leftlowerarm",
                                  "l_shoulder", "l_arm", "l_forearm", "l_upperarm", "clavicle_l", "upperarm_l",
                                  "lowerarm_l", "shoulder_l", "arm_l" },
        std::vector<std::string>{ "rightshoulder", "rightarm", "rightforearm", "rightupperarm", "rightlowerarm",
                                  "r_shoulder", "r_arm", "r_forearm", "r_upperarm", "clavicle_r", "upperarm_r",
                                  "lowerarm_r", "shoulder_r", "arm_r" },
        std::vector<std::string>{ "lefthand", "leftthumb", "leftindex", "leftmiddle", "leftring", "leftpinky",
                                  "l_hand", "hand_l", "thumb_l", "index_l", "middle_l", "ring_l", "pinky_l" },
        std::vector<std::string>{ "righthand", "rightthumb", "rightindex", "rightmiddle", "rightring", "rightpinky",
                                  "r_hand", "hand_r", "thumb_r", "index_r", "middle_r", "ring_r", "pinky_r" },
        std::vector<std::string>{ "leftupleg", "leftleg", "leftfoot", "lefttoe", "leftthigh", "leftcalf",
                                  "l_leg", "l_foot", "thigh_l", "calf_l", "foot_l", "ball_l", "upleg_l" },
        std::vector<std::string>{ "rightupleg", "rightleg", "rightfoot", "righttoe", "rightthigh", "rightcalf",
                                  "r_leg", "r_foot", "thigh_r", "calf_r", "foot_r", "ball_r", "upleg_r" },
    };
    return table;
}

} // namespace

float EvaluateAvatarMaskWeight(
    const AvatarMaskAsset& mask, std::string_view bonePath, std::string_view boneName)
{
    const float fallback = mask.defaultInclude ? 1.0f : 0.0f;
    if (mask.entries.empty()) return fallback;

    // より具体的な (bonePath が深い) エントリを優先する。
    // WHY: 「腕全体を 0 → 手だけ 1」のような上書きを、記述順に依存させないため。
    const AvatarMaskEntry* best = nullptr;
    int bestSpecificity = -1;
    int bestDepth = 0;
    for (const auto& entry : mask.entries) {
        int depth = 0;
        if (!MatchEntry(entry, bonePath, boneName, depth)) continue;
        const int specificity = PathDepth(entry.bonePath) * 1000 - depth;
        if (specificity > bestSpecificity) {
            bestSpecificity = specificity;
            best = &entry;
            bestDepth = depth;
        }
    }
    if (!best) return fallback;
    return std::clamp(RampedWeight(*best, bestDepth), 0.0f, 1.0f);
}

const char* HumanoidBodyPartName(HumanoidBodyPart part)
{
    switch (part) {
    case HumanoidBodyPart::Root:      return "Root";
    case HumanoidBodyPart::Body:      return "Body";
    case HumanoidBodyPart::Head:      return "Head";
    case HumanoidBodyPart::LeftArm:   return "Left Arm";
    case HumanoidBodyPart::RightArm:  return "Right Arm";
    case HumanoidBodyPart::LeftHand:  return "Left Hand";
    case HumanoidBodyPart::RightHand: return "Right Hand";
    case HumanoidBodyPart::LeftLeg:   return "Left Leg";
    case HumanoidBodyPart::RightLeg:  return "Right Leg";
    case HumanoidBodyPart::Count:     break;
    }
    return "Unknown";
}

const std::vector<std::string>& HumanoidBonePatterns(HumanoidBodyPart part)
{
    static const std::vector<std::string> empty;
    const auto index = static_cast<size_t>(part);
    if (index >= static_cast<size_t>(HumanoidBodyPart::Count)) return empty;
    return HumanoidPatternTable()[index];
}

bool BoneNameMatchesBodyPart(std::string_view boneName, HumanoidBodyPart part)
{
    // "mixamorig:LeftArm" のような接頭辞と区切り記号を落として比較する。
    std::string normalized;
    const std::string lower = LowerCopy(boneName);
    const size_t colon = lower.find_last_of(':');
    const std::string body = colon == std::string::npos ? lower : lower.substr(colon + 1);
    normalized.reserve(body.size());
    for (const char c : body) {
        if (c == ' ' || c == '.' || c == '-') continue;
        normalized.push_back(c);
    }

    for (const auto& pattern : HumanoidBonePatterns(part)) {
        // パターン側も区切りを持つ ("l_arm") ため、両方から '_' を除いた形でも比較する。
        std::string flatPattern;
        flatPattern.reserve(pattern.size());
        for (const char c : pattern) if (c != '_') flatPattern.push_back(c);
        std::string flatName;
        flatName.reserve(normalized.size());
        for (const char c : normalized) if (c != '_') flatName.push_back(c);

        if (normalized.find(pattern) != std::string::npos) return true;
        if (!flatPattern.empty() && flatName.find(flatPattern) != std::string::npos) return true;
    }
    return false;
}

HumanoidBodyPart GuessBodyPartForBone(std::string_view boneName)
{
    // 手 / 足先などのより具体的なパーツを先に判定する。
    // WHY: "LeftHand" は LeftArm のパターン ("leftarm") には当たらないが、
    //      "LeftHandIndex1" のような名前は Hand を優先しないと Arm に吸われる。
    static constexpr HumanoidBodyPart kOrder[] = {
        HumanoidBodyPart::LeftHand,  HumanoidBodyPart::RightHand,
        HumanoidBodyPart::LeftArm,   HumanoidBodyPart::RightArm,
        HumanoidBodyPart::LeftLeg,   HumanoidBodyPart::RightLeg,
        HumanoidBodyPart::Head,      HumanoidBodyPart::Body,
        HumanoidBodyPart::Root,
    };
    for (const HumanoidBodyPart part : kOrder)
        if (BoneNameMatchesBodyPart(boneName, part)) return part;
    return HumanoidBodyPart::Count;
}

bool SaveAvatarMaskAsset(const std::string& path, const AvatarMaskAsset& asset)
{
    toml::table root;
    toml::table header;
    header.insert("version", int64_t{ 1 });
    header.insert("name", asset.name);
    header.insert("default_include", asset.defaultInclude);
    header.insert("skeleton_source", asset.skeletonSourcePath);
    root.insert("mask", std::move(header));

    toml::array entries;
    for (const auto& entry : asset.entries) {
        toml::table t;
        t.insert("bone", entry.bonePath);
        t.insert("weight", static_cast<double>(entry.weight));
        t.insert("include_children", entry.includeChildren);
        t.insert("blend_depth", static_cast<int64_t>(entry.blendDepth));
        entries.push_back(std::move(t));
    }
    root.insert("entries", std::move(entries));

    std::ostringstream ss;
    ss << root;
    if (!util::FileSystem::WriteText(path, ss.str())) {
        FBZZ_LOG_ERROR("AvatarMask: save failed [%s]", path.c_str());
        return false;
    }
    return true;
}

bool LoadAvatarMaskAsset(const std::string& path, AvatarMaskAsset& outAsset)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    std::istringstream iss(text);
    const auto parsed = toml::parse(iss);
    if (!parsed) {
        FBZZ_LOG_WARN("AvatarMask: parse failed [%s]", path.c_str());
        return false;
    }
    const auto& table = parsed.table();

    outAsset = AvatarMaskAsset{};
    if (const auto* header = table["mask"].as_table()) {
        outAsset.name = (*header)["name"].value_or(std::string{});
        outAsset.defaultInclude = (*header)["default_include"].value_or(false);
        outAsset.skeletonSourcePath = (*header)["skeleton_source"].value_or(std::string{});
    }
    if (outAsset.name.empty())
        outAsset.name = util::FileSystem::GetFilename(path);

    if (const auto* entries = table["entries"].as_array()) {
        for (const auto& element : *entries) {
            const auto* t = element.as_table();
            if (!t) continue;
            AvatarMaskEntry entry;
            entry.bonePath = (*t)["bone"].value_or(std::string{});
            if (entry.bonePath.empty()) continue;
            entry.weight = static_cast<float>((*t)["weight"].value_or(1.0));
            entry.includeChildren = (*t)["include_children"].value_or(true);
            entry.blendDepth = static_cast<int>((*t)["blend_depth"].value_or(int64_t{ 0 }));
            entry.weight = std::clamp(entry.weight, 0.0f, 1.0f);
            entry.blendDepth = std::max(0, entry.blendDepth);
            outAsset.entries.push_back(std::move(entry));
        }
    }
    return true;
}

} // namespace fbzz::asset
