// FBZZ Engine
// ScriptComponent.hpp | fbzz::scene
// GameObject に紐付く Script インスタンスの所有者
// std::unique_ptr で 1 つの Script を保持し、owner を注入する。
// 実行順とライフサイクル呼び出しは ScriptSystem が担う。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <memory>
#include <string>

namespace fbzz::scene {

struct SerializedScriptData {
    std::string type;
    std::string fieldsToml;
    bool enabled = true;
};

struct ScriptComponent {
    std::unique_ptr<Script> script;
    // WHY: エディタ起動直後や DLL ビルド失敗時は ScriptFactory が未登録で、
    //      .fbzz 内の Script をインスタンス化できないことがある。
    //      その状態で保存しても Script 情報を失わないよう、元の type/fields を保持する。
    //      ComponentArray は固定長 dense 配列を持つため、各スロットに文字列を直置きせず必要時だけ確保する。
    std::shared_ptr<SerializedScriptData> serialized;
    bool m_awoken = false;
    bool m_started = false;
};

} // namespace fbzz::scene
