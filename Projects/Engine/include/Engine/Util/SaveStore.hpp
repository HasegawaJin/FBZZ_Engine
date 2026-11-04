/// @file    SaveStore.hpp
/// @brief   TOML backed のランタイム永続化ストア (セーブデータ / 環境設定)。
/// @author  Hasegawa Jin
/// @date    2026-08-23
/// @note DataAsset (.fzdata) は制作時の共有値、SaveStore は実行中に更新して次回起動で読む値を扱う。
/// @note 保存形式は TOML。破損調査とテストで人間が中身を読めることを優先し、他フォーマットとも揃える。
/// @note セーブ枠と環境設定は寿命が違うため、1 テーブルに同居させると「別セーブをロードしたら音量が戻った」
/// @note という不具合になる。Application が GetSaveStore() / GetConfigStore() を別々に持つ。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene { struct IScriptSerializable; }

namespace fbzz::util {

class SaveStore {
public:
    /// @param defaultPath 保存先。相対パスは実行ファイルのディレクトリ基準で解決する。
    FBZZ_ENGINE_API explicit SaveStore(std::string defaultPath);
    FBZZ_ENGINE_API ~SaveStore();

    SaveStore(const SaveStore&) = delete;
    SaveStore& operator=(const SaveStore&) = delete;
    FBZZ_ENGINE_API SaveStore(SaveStore&&) noexcept;
    FBZZ_ENGINE_API SaveStore& operator=(SaveStore&&) noexcept;

    /// @name 保存先
    /// @{
    FBZZ_ENGINE_API void SetPath(const std::string& path);
    [[nodiscard]] FBZZ_ENGINE_API const std::string& GetPath() const;
    /// @}

    /// @name ユーザー定義型
    /// @note object.Reflect() が並べたフィールドを key のテーブルとして往復させる。
    /// @note コンポーネントの Reflect と同じ書き方でセーブデータを定義できる。
    /// @{
    /// @return 常に true (メモリ上のテーブルを更新するだけで失敗しない)。ディスクへは Save() で落ちる。
    /// @note 保存先の key は Reflect の呼出し前に所有し、callback による文字列の変更を反映しない。
    FBZZ_ENGINE_API bool Write(std::string_view key, scene::IScriptSerializable& object);
    /// @note key のテーブルに無いフィールドは object の値を保つ。
    /// @note 開始時の対象テーブルを複製して読む。Reflect 中の Write/Clear/Load はストアへ残り、今回の読込み値には混在しない。
    /// @return テーブル自体が存在しなければ false (object は無変更)。
    FBZZ_ENGINE_API bool Read(std::string_view key, scene::IScriptSerializable& object);
    /// @}

    /// @name スカラー
    /// @note メモリ上のテーブルを更新するだけで、ディスクへは Save() で初めて書き出す。
    /// @note 1 フレームに何度も値を更新するゲームコードから直接ファイル I/O を走らせないため。
    /// @{
    FBZZ_ENGINE_API void SetBool  (std::string_view key, bool value);
    FBZZ_ENGINE_API void SetInt   (std::string_view key, int value);
    FBZZ_ENGINE_API void SetFloat (std::string_view key, float value);
    FBZZ_ENGINE_API void SetString(std::string_view key, std::string_view value);
    FBZZ_ENGINE_API void SetVector2(std::string_view key, const math::Vector2& value);
    FBZZ_ENGINE_API void SetVector3(std::string_view key, const math::Vector3& value);
    FBZZ_ENGINE_API void SetVector4(std::string_view key, const math::Vector4& value);

    /// @note キーが無い / 型が違う場合は defaultValue を返す。
    /// @note セーブデータはバージョン違いで欠損キーが普通に起きるため、既定値処理を呼び出し側へ毎回書かせない。
    [[nodiscard]] FBZZ_ENGINE_API bool          GetBool  (std::string_view key, bool defaultValue = false) const;
    [[nodiscard]] FBZZ_ENGINE_API int           GetInt   (std::string_view key, int defaultValue = 0) const;
    [[nodiscard]] FBZZ_ENGINE_API float         GetFloat (std::string_view key, float defaultValue = 0.0f) const;
    [[nodiscard]] FBZZ_ENGINE_API std::string   GetString(std::string_view key, std::string_view defaultValue = {}) const;
    /// @return 存在する string なら true と借用 view。欠損・型違いは false、out は未変更。
    /// @note main thread の同期転送だけに使う。Store の変更・Load/Clear/破棄で view は失効し、fallback は借用しない。
    [[nodiscard]] FBZZ_ENGINE_API bool TryGetStringView(std::string_view key, std::string_view& out) const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector2 GetVector2(std::string_view key, const math::Vector2& defaultValue = {}) const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 GetVector3(std::string_view key, const math::Vector3& defaultValue = {}) const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector4 GetVector4(std::string_view key, const math::Vector4& defaultValue = {}) const;
    /// @}

    /// @name 管理
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API bool Has(std::string_view key) const;
    FBZZ_ENGINE_API void Remove(std::string_view key);
    /// @note メモリ上のテーブルを空にする。ディスク上のファイルは Save() を呼ぶまで残る。
    FBZZ_ENGINE_API void Clear();
    /// @note 登録済みキーを辞書順で返す (デバッグ表示・セーブ一覧用)。
    [[nodiscard]] FBZZ_ENGINE_API std::vector<std::string> Keys() const;

    /// @note ディスクへ書き出す。親ディレクトリが無ければ作る。
    FBZZ_ENGINE_API bool Save();
    /// @note ディスクから読み込み、メモリ上のテーブルを置き換える。
    /// @note 真に欠損した file は空の clean 値として成功する。読込・権限・parse 失敗は false、値と dirty を維持する。
    FBZZ_ENGINE_API bool Load();
    /// @note 最後の Save() / Load() 以降に書き込みがあったか。終了確認ダイアログ等に使う。
    [[nodiscard]] FBZZ_ENGINE_API bool IsDirty() const;
    /// @return Process 固有の mutation revision。path・値・dirty の変更と store 再構築で変わる。
    [[nodiscard]] FBZZ_ENGINE_API uint64_t GetRevision() const;
    /// @return revision/path の競合は false、旧値と dirty は未変更。成功時だけ検証済み所有文字列を移す。
    FBZZ_ENGINE_API bool CommitString(std::string_view key, std::string&& value, uint64_t expectedRevision, std::string_view expectedPath);
    /// @note Portable managed 値は TOML の version/type/schema/json table として、未公開候補を一度だけ移す。
    FBZZ_ENGINE_API bool CommitDocument(std::string_view key, std::string type, std::string schema, std::string&& json,
        uint64_t expectedRevision, std::string_view expectedPath);
    /// @return portable version1 table なら true。借用 view は次の store 変更まで有効、失敗時は未変更。
    FBZZ_ENGINE_API bool TryGetDocumentView(std::string_view key, std::string_view& type, std::string_view& schema, std::string_view& json) const;
    /// @}

private:
    /// @note toml++ を公開ヘッダーへ波及させないための pImpl。
    /// @note Application.hpp から間接的に取り込まれると、ほぼ全 TU が toml++ を読むことになる。
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} /// @note namespace fbzz::util
