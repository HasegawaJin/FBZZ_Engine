/// @file    FileSystem.hpp
/// @brief   ファイル・ディレクトリ操作ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Win32 / 標準ライブラリの差を吸収し、エンジン内のパス処理を集約する。
/// 読み書き失敗は bool や空配列で返し、例外は使わない。
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef CopyFile
#undef CopyFile
#endif

#ifdef GetCurrentDirectory
#undef GetCurrentDirectory
#endif

namespace fbzz::util {

/// @note Windows.h が先に include された翻訳単位では CopyFile / GetCurrentDirectory が
///       CopyFileA/GetCurrentDirectoryA へマクロ置換され公開 API 名まで変わるため、
///       ファイル操作をこのクラスへ集約し衝突するマクロをヘッダ境界で解除する。
class FileSystem {
public:
    static bool        Exists(const std::string& path);
    /// @note const char* は std::string と std::filesystem::path の両方に暗黙変換できて曖昧になるため、string 版へ明示転送する。
    static bool        Exists(const char* path) { return Exists(std::string(path)); }
    static bool        IsDirectory(const std::string& path);
    static std::string GetExtension(const std::string& path);  ///< 例: ".scene"
    static std::string GetFilename(const std::string& path);   ///< 例: "main.scene"
    static std::string GetDirectory(const std::string& path);  ///< 例: "Assets/Scenes/"
    /// NormalizePathSeparators — Windows / POSIX の区切り文字を '/' に統一する。
    /// @note Editor / AssetBrowser / Serializer が同じ文字列表現で比較できるよう Engine 側に集約する。
    static std::string NormalizePathSeparators(std::string path, bool trimTrailingSlash = true);
    /// SamePathText — OS API に問い合わせず、正規化した文字列として同一パスかを比較する。
    /// @note 存在しない出力先や仮想 Assets パスも比較対象になるため filesystem::equivalent は使えない。
    static bool        SamePathText(const std::string& a, const std::string& b);
    /// IsChildPathText — path が root 自身、または root 配下の文字列パスかを判定する。
    /// @note AssetBrowser のマウント判定は未作成パスも扱うため、文字列だけで完結させる。
    static bool        IsChildPathText(const std::string& path, const std::string& root);

    static std::vector<std::string> ListFiles(const std::string& dir,
                                               const std::string& ext = "");
    static std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& dir);
    /// dir 以下の通常ファイルを再帰的に列挙する。
    /// @note Editor のビルド配布やホットリロード監視が同じ列挙規則を使えるようにする。
    static std::vector<std::filesystem::path> ListFilesRecursive(const std::filesystem::path& dir);
    static std::vector<std::filesystem::path> ListDirectories(const std::filesystem::path& dir);
    /// ファイルとディレクトリの両方を返す (. / .. を除く)
    static std::vector<std::string> ListAll(const std::string& dir);

    static bool EnsureDirectory(const std::string& path);
    /// @note const char* は std::string / std::filesystem::path の両方に変換でき曖昧になるため、UTF-8 として string 版へ寄せる。
    static bool EnsureDirectory(const char* path) { return EnsureDirectory(std::string(path)); }
    static bool EnsureDirectory(const std::filesystem::path& path);
    /// parent_path() が空でない場合だけ親ディレクトリを作成する。
    /// @note 呼び出し側で空親パスを特別扱いせず、ファイルコピー・書き込み前処理を共通化する。
    static bool EnsureParentDirectory(const std::filesystem::path& path);
    static bool CopyFile(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    /// Windows.h の CopyFile マクロが呼び出し側で再定義された場合の互換エイリアス。
    /// @note DirectXTex / WIC 等が本ヘッダーの後に Windows.h を取り込むと CopyFile(...) が CopyFileA/W(...) に置換されるため。
    static bool CopyFileA(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    static bool CopyFileW(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    static bool CopyDirectoryRecursive(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite = true);
    static bool RemoveAll(const std::filesystem::path& path);
    static bool Rename(const std::filesystem::path& src, const std::filesystem::path& dst);

    static bool ReadText(const std::string& path, std::string& out);
    static bool ReadText(const char* path, std::string& out) { return ReadText(std::string(path), out); }
    static bool WriteText(const std::string& path, const std::string& text);
    /// @note const char* は string / filesystem::path 双方に変換可能なため、定数文字列は string 版へ明示転送する。
    static bool WriteText(const char* path, const std::string& text) { return WriteText(std::string(path), text); }

    /// 同一フォルダのテンポラリへ書いてから置き換える。失敗時は「無傷の旧版」が残り、途中まで書けたファイルは残らない。
    /// @note ofstream は開いた瞬間に切り詰めるため、高頻度上書き (オートセーブ等) での原本破損を防ぐのに使う。
    /// @note テンポラリ名はドット始まり + .tmp。AssetBrowser の非表示規則と AssetDatabase の .meta 除外規則に乗るため、
    ///       監視下の Assets/ に置いても GUID は動かない。
    static bool WriteTextAtomic(const std::string& path, const std::string& text);
    /// 逐次バイナリ書き込み用の ofstream を開く。
    /// @note メッシュ等は構造体を順に write するため、所有する stream を呼び出し側へ返す。
    static std::ofstream OpenBinaryWriter(const std::filesystem::path& path);
    /// テクスチャやメッシュなどバイナリアセットの読み書きに使う。
    /// @note テキストでない出力も FileSystem に集約する。
    static bool ReadBinary(const std::filesystem::path& path, std::vector<uint8_t>& out);
    static bool WriteBinary(const std::filesystem::path& path, const void* data, size_t size);

    /// @name std::filesystem::path オーバーロード
    /// @{
    /// @note UTF-8 変換を呼び出し側に意識させないため path を直接受け取る版を用意する。Windows では
    ///       filesystem::path が wchar_t ベースなので変換コストもかからない。
    static bool                   Exists(const std::filesystem::path& path);
    static bool                   ReadText(const std::filesystem::path& path, std::string& out);
    static bool                   WriteText(const std::filesystem::path& path, const std::string& text);
    static std::filesystem::path  PathFromUtf8(const std::string& path);
    static std::string            PathToUtf8(const std::filesystem::path& path);
    static std::filesystem::path  RelativePath(const std::filesystem::path& path, const std::filesystem::path& root);
    static std::filesystem::path  MakeAbsolute(const std::filesystem::path& path);
    static bool                   SamePath(const std::filesystem::path& a, const std::filesystem::path& b);
    static std::filesystem::path  GetCurrentDirectory();
    /// Windows.h の GetCurrentDirectory マクロが呼び出し側で再定義された場合の互換エイリアス。
    /// @note 本ヘッダーの後に Windows.h を include する TU では GetCurrentDirectory() が GetCurrentDirectoryA/W() に置換されるため。
    static std::filesystem::path  GetCurrentDirectoryA();
    static std::filesystem::path  GetCurrentDirectoryW();

    /// 実行ファイルが存在するディレクトリを返す。
    /// @note アセット・設定ファイルの基点として複数の起動モジュールが使うため Engine に集約する。
    static std::filesystem::path  GetExecutableDirectory();

    /// ファイルまたはディレクトリの最終更新時刻を返す。存在しない場合はデフォルト値 ({}) を返す。
    static std::filesystem::file_time_type LastWriteTime(const std::filesystem::path& path);
    /// @}
};

} // namespace fbzz::util
