/// @file    SceneForest.hpp
/// @brief   準備した複数の個体階層を検証し、既存 Scene に公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {
class Scene;

/// @note 構造検証はワーカーで実行できる。Scene、アセット、Script ファクトリー、描画資源にはアクセスしない。
/// @note 保存された個体 ID を文書順に返す。失敗時は outIds を空にしてエラーを返す。
[[nodiscard]] FBZZ_ENGINE_API bool ValidateSceneForestDocument(const std::string& document,
    std::vector<std::string>& outIds, std::string& error);

/// @note メインスレッド専用。公開先を変更する前に保存 ID と階層の所有関係を検証する。
/// @note 準備はデータ処理のみ。登録済み Script のコンストラクターとコールバックは、全階層を接続した後に一度実行する。
/// @note 失敗時は両方の出力を消去する。参照する地形は読み込み成功が必要。データのみのメッシュでは resources に null を渡せる。
[[nodiscard]] FBZZ_ENGINE_API bool AppendSceneForest(Scene& destination,
    const std::string& document, const std::string& sourcePath, renderer::ResourceManager* resources,
    std::vector<EntityID>& outRoots, std::vector<EntityID>& outEntities, std::string& error);

}
