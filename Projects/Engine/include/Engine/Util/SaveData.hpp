// FBZZ Engine
// SaveData.hpp | fbzz::util
// ランタイム永続化 (キー・バリュー型のセーブデータ)
//
// 設計意図 (WHY):
//   DataAsset (.fzdata) は「オーサリング時に決めた値をランタイムで共有する」ための仕組みで、
//   ゲーム中に書き換えた内容をディスクへ戻す用途ではない (エディタセッションを汚染する)。
//   一方でハイスコア・進行度・オプション設定は「ゲームが実行中に書いて次回起動で読む」データで、
//   性質がまったく違う。両者を混ぜないよう、ランタイム永続化はこの独立した KVS に閉じる。
//
//   保存形式は TOML。バイナリにしないのは、セーブデータの破損調査とテストのために
//   人間が中身を読めることを優先したため (エンジン内の他のフォーマットとも揃う)。
//
//   スロット (セーブ枠) はパスで表現する。SetSlotPath() で切り替えてから Load() すれば
//   複数セーブに対応できる。既定は実行ファイル隣の "Saves/save0.toml"。
#pragma once
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::util {

class SaveData {
public:
    // ── スロット ──────────────────────────────────────────────────────────
    // 保存先ファイルパス。相対パスは実行ファイルのディレクトリ基準で解決する。
    static void               SetSlotPath(const std::string& path);
    static const std::string& GetSlotPath();

    // ── 書き込み ──────────────────────────────────────────────────────────
    // メモリ上のテーブルを更新するだけで、ディスクへは Save() で初めて書き出す。
    // WHY: 1 フレームに何度も値を更新するゲームコードから直接ファイル I/O を走らせないため。
    static void SetBool  (std::string_view key, bool value);
    static void SetInt   (std::string_view key, int value);
    static void SetFloat (std::string_view key, float value);
    static void SetString(std::string_view key, std::string_view value);
    static void SetVector2(std::string_view key, const math::Vector2& value);
    static void SetVector3(std::string_view key, const math::Vector3& value);
    static void SetVector4(std::string_view key, const math::Vector4& value);

    // ── 読み出し ──────────────────────────────────────────────────────────
    // キーが無い / 型が違う場合は defaultValue を返す。
    // WHY: セーブデータはバージョン違いで欠損キーが普通に起きるため、
    //      「無ければ既定値」を呼び出し側に毎回書かせない。
    [[nodiscard]] static bool          GetBool  (std::string_view key, bool defaultValue = false);
    [[nodiscard]] static int           GetInt   (std::string_view key, int defaultValue = 0);
    [[nodiscard]] static float         GetFloat (std::string_view key, float defaultValue = 0.0f);
    [[nodiscard]] static std::string   GetString(std::string_view key, std::string_view defaultValue = {});
    [[nodiscard]] static math::Vector2 GetVector2(std::string_view key, const math::Vector2& defaultValue = {});
    [[nodiscard]] static math::Vector3 GetVector3(std::string_view key, const math::Vector3& defaultValue = {});
    [[nodiscard]] static math::Vector4 GetVector4(std::string_view key, const math::Vector4& defaultValue = {});

    // ── 管理 ──────────────────────────────────────────────────────────────
    [[nodiscard]] static bool Has(std::string_view key);
    static void Remove(std::string_view key);
    // メモリ上のテーブルを空にする。ディスク上のファイルは Save() を呼ぶまで残る。
    static void Clear();
    // 登録済みキーを宣言順ではなく辞書順で返す (デバッグ表示・セーブ一覧用)。
    [[nodiscard]] static std::vector<std::string> Keys();

    // ── I/O ───────────────────────────────────────────────────────────────
    // ディスクへ書き出す。親ディレクトリが無ければ作る。
    static bool Save();
    // ディスクから読み込み、メモリ上のテーブルを置き換える。
    // WHY ファイル欠損で true を返すか: 初回起動には必ずセーブが存在しない。
    //     これを失敗として扱うと呼び出し側が毎回「初回かどうか」を分岐することになる。
    //     本当の失敗 (パース不能 = 破損) だけを false にする。
    static bool Load();
    // 最後の Save() / Load() 以降に書き込みがあったか。
    static bool IsDirty();

private:
    // Save() 時にだけ解決する。テストや GameHub からスロットを差し替えても
    // 実パス解決が 1 か所に留まるようにする。
    static std::string ResolveAbsolutePath();
};

} // namespace fbzz::util
