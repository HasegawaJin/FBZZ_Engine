/// @file    ResourceManagerLifetime.hpp
/// @brief   バックエンドの束縛所有者を生存中の Manager へ解決する内部契約。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once
#include <Graphics/Api.hpp>
#include <cstdint>
namespace fbzz::renderer {
class ResourceManager;
/// @return 終了済みまたは未登録の instanceId は nullptr。非所有参照を返す。
/// @pre Manager の生成・終了と同じ描画スレッドで呼ぶ。
/// @see Docs/design/graphics-library.md GPU 資源・終了順
[[nodiscard]] FBZZ_GRAPHICS_API ResourceManager* FindLiveResourceManager(uint64_t instanceId);
}
