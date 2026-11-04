/// @file    ModelImporter.hpp
/// @brief   Assimp を使って外部モデルを Model へ変換する入口。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note ファイル形式依存の読み取りをここに閉じ込め、Scene / Renderer には持ち込まない。
/// @note 失敗時は nullptr を返し、例外は使わない。
#pragma once
#include <Engine/Api.hpp>
#include <memory>
#include <string>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class ModelImporter {
public:
    /// @note 失敗時は nullptr を返す。所有権は呼び出し元 (AssetManager) へ移譲する。
    FBZZ_ENGINE_API static std::unique_ptr<Model> Import(
        const std::string& path,
        renderer::ResourceManager& resources);
};

} /// @note namespace fbzz::asset
