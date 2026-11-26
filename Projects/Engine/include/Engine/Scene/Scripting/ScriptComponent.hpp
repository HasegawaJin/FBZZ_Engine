/// @file    ScriptComponent.hpp
/// @brief   GameObject に紐付く Script インスタンス群の所有者。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note std::unique_ptr の配列で複数 Script を保持し、owner を注入する。
/// @note 実行順とライフサイクル呼び出しは ScriptSystem が担う。
#pragma once

#include <Engine/Scene/Scripting/Script.hpp>
#include <Engine/Util/Types/Uuid.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

struct SerializedScriptData {
    std::string type; ///< 保存された Script の型名。
    std::string fieldsToml; ///< 実体化や再読み込みに渡すフィールド値の TOML。
    bool enabled = true; ///< 保存した Script の有効状態。
    std::string savedScriptId; ///< 同じ GameObject 上で保存済み Script 個体を識別する ID。
};

struct ScriptEntry {
    ScriptEntry() = default;
    ScriptEntry(const ScriptEntry&) = delete;
    ScriptEntry& operator=(const ScriptEntry&) = delete;
    ScriptEntry(ScriptEntry&&) noexcept = default;
    ScriptEntry& operator=(ScriptEntry&&) noexcept = default;

    std::string inspectionId = util::GenerateUUID(); ///< 保存しない。vector の移動では保持し、削除・複製・Scene 復元で再発行する。
    std::string savedScriptId = util::GenerateUUID(); ///< owner GUID と組み合わせる保存 ID。複製先 owner と保存・Undo・Play 復元でも保持する。
    std::unique_ptr<Script> script; ///< このエントリーに対応する Script の実体を所有する。
    std::unique_ptr<SerializedScriptData> serialized; ///< managed catalog が未ロードのときも `.fbzz` 内の Script 情報を失わないよう、元の type/fields を保持する。
    bool m_awoken = false; ///< この実体の Awake を呼び出したか。
    bool m_started = false; ///< この実体の Start を呼び出したか。
    bool m_lifecyclePlayMode = false; ///< OnAwake を通したとき Play 中だったか (managed ExecuteAlways 用)。Play の開始・停止では Script インスタンスが作り直されないため編集中に立てた m_awoken/m_started がそのまま残り OnStart が呼ばれなくなる。初期化したモードを覚えておき、食い違ったら張り直す。
    bool m_pendingRemoval = false; ///< callback 中に個体を解放せず、Cleanup の安全点まで enabled=false のまま保持する。
};

struct ScriptComponent {
    ScriptComponent() = default;
    ScriptComponent(const ScriptComponent&) = delete;
    ScriptComponent& operator=(const ScriptComponent&) = delete;
    ScriptComponent(ScriptComponent&&) noexcept = default;
    ScriptComponent& operator=(ScriptComponent&&) noexcept = default;

    std::vector<ScriptEntry> scripts; ///< 1 GameObject に複数 Script をアタッチするための実体配列。ComponentArray は型ごとに 1 Component の dense 配列なので ScriptComponent 内部で多重化する。
};

} // namespace fbzz::scene
