// FBZZ Engine
// FileSystem.hpp | fbzz::util
// ファイル・ディレクトリ操作ユーティリティ
// Win32 / 標準ライブラリの差を吸収し、エンジン内のパス処理を集約する。
// 読み書き失敗は bool や空配列で返し、例外は使わない。
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// WHY: Windows.h が先に include された翻訳単位では CopyFile / GetCurrentDirectory などが
//      CopyFileA / GetCurrentDirectoryA へマクロ置換され、FileSystem の公開 API 名まで変わってしまう。
//      Engine 側のファイル操作はこのクラスへ集約する方針なので、衝突する Win32 マクロはヘッダ境界で解除する。
#ifdef CopyFile
#undef CopyFile
#endif

#ifdef GetCurrentDirectory
#undef GetCurrentDirectory
#endif

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
    static std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& dir);
    // dir 以下の通常ファイルを再帰的に列挙する。
    // WHY: Editor のビルド配布やホットリロード監視が同じ列挙規則を使えるようにする。
    static std::vector<std::filesystem::path> ListFilesRecursive(const std::filesystem::path& dir);
    static std::vector<std::filesystem::path> ListDirectories(const std::filesystem::path& dir);
    // ファイルとディレクトリの両方を返す (. / .. を除く)
    static std::vector<std::string> ListAll(const std::string& dir);

    static bool EnsureDirectory(const std::string& path);
    // WHY: const char* は std::string と std::filesystem::path の両方に変換できるため、
    //      リテラル呼び出しでは曖昧になる。文字列リテラルは UTF-8 として string 版へ寄せる。
    static bool EnsureDirectory(const char* path) { return EnsureDirectory(std::string(path)); }
    static bool EnsureDirectory(const std::filesystem::path& path);
    // parent_path() が空でない場合だけ親ディレクトリを作成する。
    // WHY: 呼び出し側で空親パスを特別扱いせず、ファイルコピー・書き込み前処理を共通化する。
    static bool EnsureParentDirectory(const std::filesystem::path& path);
    static bool CopyFile(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    // Windows.h の CopyFile マクロが呼び出し側で再定義された場合の互換エイリアス。
    // WHY: DirectXTex / WIC などが FileSystem.hpp の後に Windows.h を取り込むと、
    //      util::FileSystem::CopyFile(...) も util::FileSystem::CopyFileA/W(...) に置換される。
    static bool CopyFileA(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    static bool CopyFileW(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    static bool CopyDirectoryRecursive(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    static bool RemoveAll(const std::filesystem::path& path);
    static bool Rename(const std::filesystem::path& src, const std::filesystem::path& dst);

    static bool ReadText(const std::string& path, std::string& out);
    static bool ReadText(const char* path, std::string& out) { return ReadText(std::string(path), out); }
    static bool WriteText(const std::string& path, const std::string& text);
    // WHY: const char* パスは std::string / std::filesystem::path の両方へ変換可能なので、
    //      kSnapshotPath のような定数文字列は string 版へ明示的に転送する。
    static bool WriteText(const char* path, const std::string& text) { return WriteText(std::string(path), text); }
    // 逐次バイナリ書き込み用の ofstream を開く。
    // WHY: メッシュ等は構造体を順に write するため、所有する stream は呼び出し側へ返す。
    static std::ofstream OpenBinaryWriter(const std::filesystem::path& path);
    // 任意バイト列をそのままファイルへ書き出す。
    // WHY: テクスチャやメッシュなどテキストでないアセット出力も FileSystem に集約する。
    static bool ReadBinary(const std::filesystem::path& path, std::vector<uint8_t>& out);
    static bool WriteBinary(const std::filesystem::path& path, const void* data, size_t size);

    // --- std::filesystem::path オーバーロード ---
    // WHY: 呼び出し側が UTF-8 変換を意識しなくて済むよう、path を直接受け取る版を用意する。
    //      Windows では filesystem::path が wchar_t ベースなので変換コストもかからない。
    static bool                   Exists(const std::filesystem::path& path);
    static bool                   ReadText(const std::filesystem::path& path, std::string& out);
    static bool                   WriteText(const std::filesystem::path& path, const std::string& text);
    static std::filesystem::path  PathFromUtf8(const std::string& path);
    static std::string            PathToUtf8(const std::filesystem::path& path);
    static std::filesystem::path  RelativePath(const std::filesystem::path& path, const std::filesystem::path& root);
    static std::filesystem::path  MakeAbsolute(const std::filesystem::path& path);
    static bool                   SamePath(const std::filesystem::path& a, const std::filesystem::path& b);
    static std::filesystem::path  GetCurrentDirectory();
    // Windows.h の GetCurrentDirectory マクロが呼び出し側で再定義された場合の互換エイリアス。
    // WHY: FileSystem.hpp の後に Windows.h を include する翻訳単位では、
    //      FileSystem::GetCurrentDirectory() が FileSystem::GetCurrentDirectoryA/W() に置換される。
    static std::filesystem::path  GetCurrentDirectoryA();
    static std::filesystem::path  GetCurrentDirectoryW();

    /// 実行ファイルが存在するディレクトリを返す。
    /// WHY: アセット・設定ファイルの基点として複数の起動モジュールが使うため Engine に集約する。
    static std::filesystem::path  GetExecutableDirectory();
};

} // namespace fbzz::util
