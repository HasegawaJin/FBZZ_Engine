// FBZZ Engine
// ScriptSaveProxy.hpp | fbzz::scene
// Script からランタイム永続化 (セーブデータ) を読み書きする
//
// 設計意図 (WHY):
//   Script は Engine 実装へ直接依存させない方針なので、util::SaveData をそのまま
//   include させず、他のプロキシと同じ形で薄く転送する。
//
//   Save() を明示的に呼ばせるのは意図的。オート保存にすると「どのタイミングで
//   ディスクに落ちたか」がゲーム側から見えなくなり、チェックポイント演出
//   (セーブ中アイコンの表示など) が書けなくなる。
#pragma once

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptSaveProxy {
    Script* script = nullptr;

    // ── スロット ──────────────────────────────────────────────────────────
    // セーブ枠の切り替え。切り替えた後に Load() を呼ぶこと。
    void SetSlot(std::string_view path) const;
    [[nodiscard]] std::string GetSlot() const;

    // ── 値の読み書き (メモリ上) ───────────────────────────────────────────
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

    [[nodiscard]] bool Has(std::string_view key) const;
    void Remove(std::string_view key) const;
    void Clear() const;

    // ── ディスク I/O ──────────────────────────────────────────────────────
    // Save: 書き出し。Load: 読み込み (ファイルが無い初回起動も true)。
    bool Save() const;
    bool Load() const;
    // 最後の Save / Load 以降に書き込みがあったか。終了確認ダイアログ等に使う。
    [[nodiscard]] bool IsDirty() const;
};

} // namespace fbzz::scene
