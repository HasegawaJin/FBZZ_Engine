/// @file    ScriptValidation.hpp
/// @brief   FBZZ_REQUIRE_COMPONENT / FBZZ_OPTIONAL_COMPONENT の充足検査。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY Engine 側に置くか:
/// 同じ判定を 3 箇所が必要とする — Inspector の赤帯、Play 開始時のシーン一括検証、
/// ScriptSystem の実行時警告。Editor に置くと最後の 1 つ (Standalone ビルド) が
/// 検証を失い、「エディタでは怒られるのに製品ビルドでは無言で動かない」という
/// 一番混乱する食い違いが生まれる。判定はここに一本化する。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Scene;
class Script;

// 「このスクリプトに、この GameObject では足りていないコンポーネントがある」1 件。
struct ScriptRequirementIssue {
    EntityID    entity = EntityID::INVALID;
    std::string objectName;      // GameObject の表示名 (ログ用)
    std::string instanceId;      // GUID。EntityID は再ロードで振り直るため選択の復元に使う
    std::string scriptType;      // 要求した Script の型名
    std::string componentType;   // 不足しているコンポーネントの型名 (serializedName)
    std::string componentDisplay;// 人間向け表示名。未登録型なら componentType と同じ
    // FBZZ_OPTIONAL_COMPONENT 由来。エラーではなく情報として扱う。
    bool optional = false;
    // ComponentRegistry に存在しない型名。宣言側の誤りなので、
    // 「足せば直る」ものではない (Fix ボタンを出してはいけない)。
    bool unknown = false;
    // 登録されてはいるが Add Component メニューに出ない内部型。手では足せない。
    bool addable = false;
};

// script が要求するもののうち go に足りていないものを out へ追加する。
// includeOptional=false のとき FBZZ_OPTIONAL_COMPONENT ぶんは収集しない。
void CollectScriptRequirementIssues(GameObject& go,
                                    const Script& script,
                                    std::vector<ScriptRequirementIssue>& out,
                                    bool includeOptional = false);

// シーン内の全 ScriptComponent を走査して不足を集める。Play 開始前の一括検証用。
[[nodiscard]] std::vector<ScriptRequirementIssue> ValidateSceneScriptRequirements(
    Scene& scene, bool includeOptional = false);

// 1 件を人間が読める 1 行にする。Console / Toast / Inspector で文面を揃えるため、
// 整形はここに集約する。
[[nodiscard]] std::string FormatScriptRequirementIssue(const ScriptRequirementIssue& issue);

} // namespace fbzz::scene
