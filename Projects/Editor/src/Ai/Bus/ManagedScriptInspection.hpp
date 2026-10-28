/// @file    ManagedScriptInspection.hpp
/// @brief   managed 個体のフィールドだけを安全に観測する AI 投影。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <string>

namespace fbzz::scene {
class ManagedScriptAdapter;
class Scene;
}

namespace fbzz::editor::ai {
class JsonValue;
}

namespace fbzz::editor::ai::bus {

/// @note main thread の Query 専用。callback・Reflect・property getter・書き込みを実行しない。
void InspectManagedFields(scene::ManagedScriptAdapter& adapter, scene::Scene& scene, JsonValue& result);
/// @note snapshot の path/range を Host で取得する。close は値を変更せず保持資源だけを解放する。
bool InspectManagedPage(scene::ManagedScriptAdapter& adapter, const JsonValue& options, JsonValue& result, std::string& diagnostic);

/// @note 生成済み catalog 個体の既定値を同じ field codec で読む。constructor・callback・getter を再実行しない。
void CatalogManagedFields(scene::ManagedScriptAdapter& adapter, JsonValue& result);

} /// @note namespace fbzz::editor::ai::bus
