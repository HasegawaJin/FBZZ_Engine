/// @file    ScriptComponent.hpp
/// @brief   GameObject に紐付く Script インスタンス群の所有者。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// std::unique_ptr の配列で複数 Script を保持し、owner を注入する。
/// 実行順とライフサイクル呼び出しは ScriptSystem が担う。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Engine/Util/Uuid.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

struct SerializedScriptData {
    std::string type;
    std::string fieldsToml;
    bool enabled = true;
};

struct ScriptEntry {
    ScriptEntry() = default;
    ScriptEntry(const ScriptEntry&) = delete;
    ScriptEntry& operator=(const ScriptEntry&) = delete;
    ScriptEntry(ScriptEntry&&) noexcept = default;
    ScriptEntry& operator=(ScriptEntry&&) noexcept = default;

    /// @note 保存しない。vector の移動では保持し、削除・複製・Scene 復元で再発行する。
    std::string inspectionId = util::GenerateUUID();
    std::unique_ptr<Script> script;
    /// @note エディタ起動直後や DLL ビルド失敗時は ScriptFactory が未登録で `.fbzz` 内の Script をインスタンス化できないことがある。その状態で保存しても情報を失わないよう元の type/fields を保持する。
    std::unique_ptr<SerializedScriptData> serialized;
    bool m_awoken = false;
    bool m_started = false;
    /// @note OnAwake を通したとき Play 中だったか (FBZZ_EXECUTE_ALWAYS 用)。Play の開始・停止では Script インスタンスが作り直されないため編集中に立てた m_awoken/m_started がそのまま残り OnStart が呼ばれなくなる。初期化したモードを覚えておき、食い違ったら張り直す。
    bool m_lifecyclePlayMode = false;
};

struct ScriptComponent {
    ScriptComponent() = default;
    ScriptComponent(const ScriptComponent&) = delete;
    ScriptComponent& operator=(const ScriptComponent&) = delete;
    ScriptComponent(ScriptComponent&&) noexcept = default;
    ScriptComponent& operator=(ScriptComponent&&) noexcept = default;

    /// @note 1 GameObject に複数 Script をアタッチするための実体配列。ComponentArray は型ごとに 1 Component の dense 配列なので ScriptComponent 内部で多重化する。
    std::vector<ScriptEntry> scripts;
};

} /// namespace fbzz::scene
