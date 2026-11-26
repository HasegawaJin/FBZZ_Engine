/// @file    ScriptValidation.hpp
/// @brief   managed RequireComponentAttribute の充足検査。
/// @author  Hasegawa Jin
/// @date    2026-08-19

/// @note Inspector の赤帯・Play 開始時のシーン一括検証・ScriptSystem の実行時警告の 3 箇所が同じ判定を必要とする。Editor に置くと Standalone ビルドだけ検証を失うため Engine 側に一本化する。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/References/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Scene;
class Script;

/// @brief 「このスクリプトに、この GameObject では足りていないコンポーネントがある」1 件。
enum class ScriptRequirementKind { Component, Script, Asset, Reference };

struct ScriptRequirementIssue {
    ScriptRequirementKind kind = ScriptRequirementKind::Component; ///< 不足または不正だった前提条件の種類。
    std::string fieldKey; ///< 問題のある公開設定フィールドのキー。
    std::string reason; ///< 不足や不正を利用者に示す診断文。
    bool blocksStart = false; ///< この問題が Script の実行開始を妨げるか。
    EntityID    entity = EntityID::INVALID; ///< 問題のある Script を所有する個体の Entity ID。
    std::string objectName; ///< GameObject の表示名 (ログ用)
    std::string instanceId; ///< GUID。EntityID は再ロードで振り直るため選択の復元に使う
    std::string scriptType; ///< 要求した Script の型名
    std::string componentType; ///< 不足しているコンポーネントの型名 (serializedName)
    std::string componentDisplay;///< @note 人間向け表示名。未登録型なら componentType と同じ
    bool optional = false; ///< optional requirement は情報として扱う。
    bool unknown = false; ///< ComponentRegistry に存在しない型名。宣言側の誤りで「足せば直る」ものではない (Fix ボタンを出してはいけない)。
    bool addable = false; ///< 登録されてはいるが Add Component メニューに出ない内部型。手では足せない。
};

/// @brief script が要求するもののうち go に足りていないものを out へ追加する。
/// @param[in] includeOptional false のとき optional requirement は収集しない。
FBZZ_ENGINE_API void CollectScriptRequirementIssues(GameObject& go,
                                    const Script& script,
                                    std::vector<ScriptRequirementIssue>& out,
                                    bool includeOptional = false, bool resolveAssets = false);

/// @brief 必須設定を実行前に解決し、不足時は当該 Script の開始を止める。
[[nodiscard]] FBZZ_ENGINE_API bool ValidateScriptRequirementsForStart(GameObject& go, Script& script);

/// @brief シーン内の全 ScriptComponent を走査して不足を集める。Play 開始前の一括検証用。
[[nodiscard]] FBZZ_ENGINE_API std::vector<ScriptRequirementIssue> ValidateSceneScriptRequirements(
    Scene& scene, bool includeOptional = false);

/// @brief 1 件を人間が読める 1 行にする。
/// @note Console/Toast/Inspector で文面を揃えるため、整形をここに集約する。
[[nodiscard]] FBZZ_ENGINE_API std::string FormatScriptRequirementIssue(const ScriptRequirementIssue& issue);

} // namespace fbzz::scene
