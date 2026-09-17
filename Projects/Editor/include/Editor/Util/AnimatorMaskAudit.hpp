/// @file    AnimatorMaskAudit.hpp
/// @brief   複数レイヤーのマスクを重ねた結果を数値化し、破綻を検出する
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 画面に出るのは layerWeight × maskWeight を Override レイヤー順に補間した結果で、Base Layer の残り方はどの .mask にも書かれていない。
/// @note 合成規則をここへ 1 本化し、可視化 (Composition) と警告 (Audit) が同じ計算を見る。
#pragma once

#include <Engine/Asset/AvatarMaskAsset.hpp>

#include <string>
#include <vector>

namespace fbzz::asset {
struct Skeleton;
} // namespace fbzz::asset

namespace fbzz::scene {
struct AnimatorComponent;
} // namespace fbzz::scene

namespace fbzz::editor::maskaudit {

/// マスクを解決済みにした 1 レイヤーぶんの定義。
struct LayerInfo {
    std::string name;
    bool        additive = false;
    bool        enabled  = true;
    float       weight   = 1.0f;
    /// nullptr ならマスクなし = 全ボーンに 1.0 で効く。
    const asset::AvatarMaskAsset* mask = nullptr;
    std::string maskPath;
    /// path は入っているのに読めなかった。全身へ効く扱いになるため区別する。
    bool maskLoadFailed = false;
};

/// 1 ボーンぶんの合成結果。baseShare と share[] の総和が 1.0 になる。
struct BoneContribution {
    float              baseShare = 1.0f;
    std::vector<float> share;         ///< layers と同じ並び。Additive レイヤーは常に 0
    std::vector<float> additiveGain;  ///< layers と同じ並び。Override レイヤーは常に 0
    /// share が最大のレイヤー番号。Base Layer が最大なら -1。
    int owner = -1;
};

enum class Severity { Info, Warning };

struct Issue {
    Severity    severity = Severity::Warning;
    std::string layerName;
    std::string boneName;
    std::string message;
};

/// Animator のレイヤー定義を取り出す。未ロードの .mask はここで読み込む
/// (AnimatorSystem は再生中しかマスクを読まないため、編集中は誰も読んでいない)。
[[nodiscard]] std::vector<LayerInfo> CollectLayers(scene::AnimatorComponent& animator);

/// ボーン 1 本ぶんの取り分を、レイヤー順に補間を畳んで求める。
[[nodiscard]] BoneContribution Evaluate(const std::vector<LayerInfo>& layers,
                                        const std::string& bonePath,
                                        const std::string& boneName);

/// スケルトン全体を走査して破綻を集める。skeleton が空なら何も返さない。
[[nodiscard]] std::vector<Issue> Audit(const asset::Skeleton& skeleton,
                                       const std::vector<LayerInfo>& layers);

/// blendDepth が作るランプを "0.33 / 0.67 / 1.00" の形で返す (深さ 0 から blendDepth まで)。
/// @note AvatarMaskRampedWeight は指定した骨自身が weight に届かない規則。
///       Inspector の数値入力欄からは読み取れないため、ここでそのまま返す。
[[nodiscard]] std::vector<float> BlendDepthRamp(float weight, int blendDepth);

/// スケルトンのうち、そのマスクが 1 つでもエントリで拾えたボーンの本数。
/// 0 ならボーン名が骨格と噛み合っていない (レイヤーが完全に無効になる)。
[[nodiscard]] int CountMaskedBones(const asset::Skeleton& skeleton,
                                   const asset::AvatarMaskAsset& mask);

} // namespace fbzz::editor::maskaudit
