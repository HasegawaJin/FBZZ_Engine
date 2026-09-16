/// @file    SaveStore.hpp
/// @brief   TOML backed のランタイム永続化ストア (セーブデータ / 環境設定)。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 設計意図 (WHY):
///   DataAsset (.fzdata) は「オーサリング時に決めた値をランタイムで共有する」ための仕組みで、
///   ゲーム中に書き換えた内容をディスクへ戻す用途ではない (エディタセッションを汚染する)。
///   ハイスコア・進行度・オプション設定は「ゲームが実行中に書いて次回起動で読む」データで、
///   性質がまったく違う。両者を混ぜないよう、ランタイム永続化はこのストアに閉じる。
///
///   保存形式は TOML。バイナリにしないのは、セーブデータの破損調査とテストのために
///   人間が中身を読めることを優先したため (エンジン内の他のフォーマットとも揃う)。
///
///   WHY static クラスではなくインスタンスか:
///     セーブ枠 (進行) と環境設定 (Option) は寿命が違う。SetPath + Load はテーブルを
///     まるごと置き換えるため、1 本のテーブルに同居させると「別のセーブをロードしたら
///     音量が戻った」という、原因の見えない不具合になる。Application が
///     GetSaveStore() / GetConfigStore() の 2 本を別々に持つ。
#pragma once

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene { struct IScriptSerializable; }

namespace fbzz::util {

class SaveStore {
public:
    /// @param defaultPath 保存先。相対パスは実行ファイルのディレクトリ基準で解決する。
    explicit SaveStore(std::string defaultPath);
    ~SaveStore();

    SaveStore(const SaveStore&) = delete;
    SaveStore& operator=(const SaveStore&) = delete;
    SaveStore(SaveStore&&) noexcept;
    SaveStore& operator=(SaveStore&&) noexcept;

    /// @name 保存先
    ///@{
    void SetPath(const std::string& path);
    [[nodiscard]] const std::string& GetPath() const;
    ///@}

    /// @name ユーザー定義型
    /// object.Reflect() が並べたフィールドを key のテーブルとして往復させる。
    /// コンポーネントの Reflect と同じ書き方でセーブデータを定義できる。
    ///@{
    /// @ret 常に true (メモリ上のテーブルを更新するだけで失敗しない)。ディスクへは Save() で落ちる。
    bool Write(std::string_view key, scene::IScriptSerializable& object);
    /// key のテーブルに無いフィールドは object の値を保つ。
    /// @ret テーブル自体が存在しなければ false (object は無変更)。
    bool Read(std::string_view key, scene::IScriptSerializable& object);
    ///@}

    /// @name スカラー
    /// メモリ上のテーブルを更新するだけで、ディスクへは Save() で初めて書き出す。
    /// WHY: 1 フレームに何度も値を更新するゲームコードから直接ファイル I/O を走らせないため。
    ///@{
    void SetBool  (std::string_view key, bool value);
    void SetInt   (std::string_view key, int value);
    void SetFloat (std::string_view key, float value);
    void SetString(std::string_view key, std::string_view value);
    void SetVector2(std::string_view key, const math::Vector2& value);
    void SetVector3(std::string_view key, const math::Vector3& value);
    void SetVector4(std::string_view key, const math::Vector4& value);

    // キーが無い / 型が違う場合は defaultValue を返す。
    // WHY: セーブデータはバージョン違いで欠損キーが普通に起きるため、
    //      「無ければ既定値」を呼び出し側に毎回書かせない。
    [[nodiscard]] bool          GetBool  (std::string_view key, bool defaultValue = false) const;
    [[nodiscard]] int           GetInt   (std::string_view key, int defaultValue = 0) const;
    [[nodiscard]] float         GetFloat (std::string_view key, float defaultValue = 0.0f) const;
    [[nodiscard]] std::string   GetString(std::string_view key, std::string_view defaultValue = {}) const;
    [[nodiscard]] math::Vector2 GetVector2(std::string_view key, const math::Vector2& defaultValue = {}) const;
    [[nodiscard]] math::Vector3 GetVector3(std::string_view key, const math::Vector3& defaultValue = {}) const;
    [[nodiscard]] math::Vector4 GetVector4(std::string_view key, const math::Vector4& defaultValue = {}) const;
    ///@}

    /// @name 管理
    ///@{
    [[nodiscard]] bool Has(std::string_view key) const;
    void Remove(std::string_view key);
    /// メモリ上のテーブルを空にする。ディスク上のファイルは Save() を呼ぶまで残る。
    void Clear();
    /// 登録済みキーを辞書順で返す (デバッグ表示・セーブ一覧用)。
    [[nodiscard]] std::vector<std::string> Keys() const;

    /// ディスクへ書き出す。親ディレクトリが無ければ作る。
    bool Save();
    /// ディスクから読み込み、メモリ上のテーブルを置き換える。
    /// WHY ファイル欠損で true を返すか: 初回起動には必ずファイルが存在しない。
    ///     これを失敗として扱うと呼び出し側が毎回「初回かどうか」を分岐することになる。
    ///     本当の失敗 (パース不能 = 破損) だけを false にする。
    bool Load();
    /// 最後の Save() / Load() 以降に書き込みがあったか。終了確認ダイアログ等に使う。
    [[nodiscard]] bool IsDirty() const;
    ///@}

private:
    // toml++ を公開ヘッダーへ波及させないための pImpl。
    // Application.hpp から間接的に取り込まれると、ほぼ全 TU が toml++ を読むことになる。
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fbzz::util
