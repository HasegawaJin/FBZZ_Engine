/// @file    ModelImporter.hpp
/// @brief   Assimp を使って外部モデルを Model へ変換する入口。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// ファイル形式依存の読み取りをここに閉じ込め、Scene / Renderer には持ち込まない。
/// 失敗時は nullptr を返し、例外は使わない。
#pragma once
#include <memory>
#include <string>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class ModelImporter {
public:
    // 失敗時は nullptr を返す。所有権は呼び出し元 (AssetManager) へ移譲する。
    static std::unique_ptr<Model> Import(
        const std::string& path,
        renderer::ResourceManager& resources);
};

} // namespace fbzz::asset
