/// @file    TempDir.hpp
/// @brief   テスト終了時に消える一時ディレクトリ。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// シリアライザのテストはファイルを書く。カレントディレクトリや Assets/ に直接書くと、
/// 途中で落ちたときにゴミが残り、次の実行が «前回の残骸» を読んで通ってしまう。
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::testkit {

class TempDir {
public:
    /// label はディレクトリ名に含める。落ちて残ったときにどのテストのものか分かるようにする。
    explicit TempDir(const std::string& label = "fbzz");
    ~TempDir();

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const { return m_path; }

    /// ディレクトリ配下のパスを作る。ファイルはまだ存在しない。
    std::filesystem::path File(const std::string& name) const { return m_path / name; }

    /// 生成に失敗していれば false。失敗したテストは ASSERT で早期に止めること。
    bool IsValid() const { return m_valid; }

private:
    std::filesystem::path m_path;
    bool                  m_valid = false;
};

} // namespace fbzz::testkit
