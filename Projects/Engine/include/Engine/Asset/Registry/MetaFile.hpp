/// @file    MetaFile.hpp
/// @brief   アセットメタデータのLF保存と同値な再保存の抑止。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#pragma once
#include <Engine/Api.hpp>
#include <string>

namespace fbzz::asset {

class MetaFile final {
public:
    /// @brief UTF-8 TOMLをLFで原子的に保存する。
    /// @return 内容が同じで書き込み不要の場合もtrue。読取・解析・保存失敗時はfalseで既存ファイルを保持する。
    /// @note TOMLが同値なら既存のコメント・書式を保ち、必要な場合だけ改行をLFへ変える。
    [[nodiscard]] FBZZ_ENGINE_API static bool Save(const std::string& path, const std::string& text);
};

} /// @note namespace fbzz::asset
