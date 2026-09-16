/// @file    ScriptSaveProxy.hpp
/// @brief   Script からランタイム永続化 (セーブデータ / 環境設定) を読み書きする。
/// @author  Hasegawa Jin
/// @date    2025-01-01
///
/// 設計意図 (WHY):
///   Script は Engine 実装へ直接依存させない方針なので、util::SaveStore をそのまま
///   include させず、他のプロキシと同じ形で薄く転送する。
///
///   Save() を明示的に呼ばせるのは意図的。オート保存にすると「どのタイミングで
///   ディスクに落ちたか」がゲーム側から見えなくなり、チェックポイント演出
///   (セーブ中アイコンの表示など) が書けなくなる。
///
///   WHY save と config を分けるか:
///     セーブ枠の切り替え (SetSlot + Load) はテーブルを丸ごと置き換える。音量や解像度を
///     save 側へ書くと、別のセーブをロードした瞬間に設定が消える。進行データと
///     環境設定はストアごと分ける。
///
///   ユーザー定義型は Write / Read で往復させる。コンポーネントの Reflect と
///   同じ書き方なので、覚えることが増えない:
///
///     struct PlayerSave : scene::IScriptSerializable {
///         int level = 1;
///         std::vector<std::string> items;
///         void Reflect(scene::IReflector& r) override {
///             r.Field("level", level);
///             r.ListField("items", items);
///         }
///     };
///     save.Write("player", data);  save.Save();
///
/// 詳細は Docs/design/game-settings.md。
#pragma once

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;
struct IScriptSerializable;

/// どちらの永続化ストアを指すか。
/// WHY プロキシごとに別の型を作らないか: API はまったく同じで、違うのは保存先だけ。
///     2 つ書くと、片方にだけメソッドを足す形の取りこぼしが起きる。
enum class SaveStoreKind : uint8_t {
    Slot,     ///< 進行データ。SetPath でセーブ枠を切り替える
    Config,   ///< 環境設定。枠に依らず常に 1 本
};

/// save / config が共有する実体。仮想関数を持たないので DLL 境界を跨いでも安全。
struct ScriptStoreProxyBase {
    Script*       script = nullptr;
    SaveStoreKind kind   = SaveStoreKind::Slot;

    /// @name 保存先
    ///@{
    void SetPath(std::string_view path) const;
    [[nodiscard]] std::string GetPath() const;
    ///@}

    /// @name ユーザー定義型
    /// object.Reflect() が並べたフィールドを key のテーブルとして往復させる。
    ///@{
    bool Write(std::string_view key, IScriptSerializable& object) const;
    /// key のテーブルに無いフィールドは object の値を保つ (フィールド追加は前方互換)。
    /// @ret テーブル自体が無ければ false。object は変更しない。
    bool Read(std::string_view key, IScriptSerializable& object) const;
    ///@}

    /// @name スカラー
    ///@{
    void SetBool   (std::string_view key, bool value) const;
    void SetInt    (std::string_view key, int value) const;
    void SetFloat  (std::string_view key, float value) const;
    void SetString (std::string_view key, std::string_view value) const;
    void SetVector2(std::string_view key, const math::Vector2& value) const;
    void SetVector3(std::string_view key, const math::Vector3& value) const;
    void SetVector4(std::string_view key, const math::Vector4& value) const;

    [[nodiscard]] bool          GetBool   (std::string_view key, bool defaultValue = false) const;
    [[nodiscard]] int           GetInt    (std::string_view key, int defaultValue = 0) const;
    [[nodiscard]] float         GetFloat  (std::string_view key, float defaultValue = 0.0f) const;
    [[nodiscard]] std::string   GetString (std::string_view key, std::string_view defaultValue = {}) const;
    [[nodiscard]] math::Vector2 GetVector2(std::string_view key, const math::Vector2& defaultValue = {}) const;
    [[nodiscard]] math::Vector3 GetVector3(std::string_view key, const math::Vector3& defaultValue = {}) const;
    [[nodiscard]] math::Vector4 GetVector4(std::string_view key, const math::Vector4& defaultValue = {}) const;
    ///@}

    /// @name 管理
    ///@{
    [[nodiscard]] bool Has(std::string_view key) const;
    void Remove(std::string_view key) const;
    void Clear() const;

    /// Save: 書き出し。Load: 読み込み (ファイルが無い初回起動も true)。
    bool Save() const;
    bool Load() const;
    /// 最後の Save / Load 以降に書き込みがあったか。終了確認ダイアログ等に使う。
    [[nodiscard]] bool IsDirty() const;
    ///@}
};

/// 進行データ。既定の保存先は実行ファイル隣の "Saves/save0.toml"。
struct ScriptSaveProxy : ScriptStoreProxyBase {
    explicit ScriptSaveProxy(Script* owner)
    {
        script = owner;
        kind   = SaveStoreKind::Slot;
    }

    /// セーブ枠の切り替え。切り替えた後に Load() を呼ぶこと。
    void SetSlot(std::string_view path) const { SetPath(path); }
    [[nodiscard]] std::string GetSlot() const { return GetPath(); }
};

/// 環境設定 (Option)。既定の保存先は実行ファイル隣の "Config/settings.toml"。
///
/// エンジンはこの中身を解釈しない。キー名も構造もゲームが決める。
/// WHY: エンジンが読むなら "fullscreen" というキー名をエンジンが固定することになる。
///      機構はエンジン、方針はゲーム、で揃える (Docs/design/game-settings.md §10.1)。
struct ScriptConfigProxy : ScriptStoreProxyBase {
    explicit ScriptConfigProxy(Script* owner)
    {
        script = owner;
        kind   = SaveStoreKind::Config;
    }
};

} // namespace fbzz::scene
