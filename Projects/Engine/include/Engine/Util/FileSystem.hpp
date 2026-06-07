// FBZZ Engine
// FileSystem.hpp | fbzz::util
// ファイル・ディレクトリ操作ユーティリティ
// Win32 / 標準ライブラリの差を吸収し、エンジン内のパス処理を集約する。
// 読み書き失敗は bool や空配列で返し、例外は使わない。
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::util {

class FileSystem {
public:
    static bool        Exists(const std::string& path);
    // WHY: const char* リテラルは std::string と std::filesystem::path の両方に暗黙変換できるため
    //      オーバーロードが曖昧になる。string 版へ明示的に転送することで解決する。
    static bool        Exists(const char* path) { return Exists(std::string(path)); }
    static bool        IsDirectory(const std::string& path);
    static std::string GetExtension(const std::string& path);  // 例: ".fbzz"
    static std::string GetFilename(const std::string& path);   // 例: "scene.fbzz"
    static std::string GetDirectory(const std::string& path);  // 例: "Assets/Scenes/"
    // NormalizePathSeparators — Windows / POSIX の区切り文字を '/' に統一する。
    // WHY: Editor / AssetBrowser / Serializer が同じ文字列表現で比較できるよう、Engine 側に集約する。
    static std::string NormalizePathSeparators(std::string path, bool trimTrailingSlash = true);
    // SamePathText — OS API に問い合わせず、正規化した文字列として同一パスかを比較する。
    // WHY: 存在しない出力先や仮想 Assets パスも比較対象になるため、filesystem::equivalent は使えない。
    static bool        SamePathText(const std::string& a, const std::string& b);
    // IsChildPathText — path が root 自身、または root 配下の文字列パスかを判定する。
    // WHY: AssetBrowser のマウント判定では未作成パスも扱うため、文字列だけで完結させる。
    static bool        IsChildPathText(const std::string& path, const std::string& root);

    static std::vector<std::string> ListFiles(const std::string& dir,
                                               const std::string& ext = "");
    // ファイルとディレクトリの両方を返す (. / .. を除く)
    static std::vector<std::string> ListAll(const std::string& dir);

    static bool EnsureDirectory(const std::string& path);

    static bool ReadText(const std::string& path, std::string& out);
    static bool ReadText(const char* path, std::string& out) { return ReadText(std::string(path), out); }
    static bool WriteText(const std::string& path, const std::string& text);

    // --- std::filesystem::path オーバーロード ---
    // WHY: 呼び出し側が UTF-8 変換を意識しなくて済むよう、path を直接受け取る版を用意する。
    //      Windows では filesystem::path が wchar_t ベースなので変換コストもかからない。
    static bool                   Exists(const std::filesystem::path& path);
    static bool                   ReadText(const std::filesystem::path& path, std::string& out);
    static std::filesystem::path  MakeAbsolute(const std::filesystem::path& path);

    /// 実行ファイルが存在するディレクトリを返す。
    /// WHY: アセット・設定ファイルの基点として複数の起動モジュールが使うため Engine に集約する。
    static std::filesystem::path  GetExecutableDirectory();
};

} // namespace fbzz::util
