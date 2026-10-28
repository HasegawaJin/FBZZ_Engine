/// @file    ManagedSettingsCodec.hpp
/// @brief   C# の保存値と通常の Configure JSON を共通の値に投影する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Editor/Op/OpData.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { struct IScriptSerializable; }
namespace fbzz::editor {
/// @note 件数・総 byte 数を表示 packet の都合で制限しない。64 段の構造制約は保存 schema と別途照合する。
/// @note 重複 key・非有限数値・不正 Unicode を拒否し、失敗時 output は未変更。
/// @see https://www.rfc-editor.org/rfc/rfc8259 JSON の数値・文字列・object 契約。
[[nodiscard]] bool ReadManagedSettingsJson(std::string_view json, OpData& output);
/// @note Compares returned values/container shape and exact reference identities, ignoring target/sampling metadata; equality covers only the returned page.
[[nodiscard]] bool ManagedSettingsPagesEqual(const OpData& first, const OpData& second);
/// @brief Retains the actual start offset returned for each Inspector page.
class ManagedSettingsPageCursor final {
public:
    /// @return Current request offset; the first page starts at zero.
    [[nodiscard]] std::uint64_t Start() const;
    /// @return True when the current page has a valid nextStart and the cursor advanced.
    [[nodiscard]] bool CanAdvance(const OpData& page) const;
    /// @return True when the page describes a valid nextStart and the cursor advanced.
    [[nodiscard]] bool Advance(const OpData& page);
    /// @return True when a previously visited page exists and the cursor moved back.
    [[nodiscard]] bool Back();
    /// @brief Drops visited pages after the current one when an edit changes page boundaries.
    void InvalidateFuture();
    /// @brief Returns to the first page.
    void Reset();
    /// @return One-based page number in the currently known navigation history.
    [[nodiscard]] std::size_t PageNumber() const { return m_current + 1; }
    /// @return Whether a page following the current one is already known.
    [[nodiscard]] bool HasVisitedNext() const { return m_current + 1 < m_starts.size(); }
private:
    std::vector<std::uint64_t> m_starts{0};
    std::size_t m_current = 0;
};
/// @note Native の json/objectGuids 投影を変更せず、version 1 envelope の value だけを Configure へ渡す。
[[nodiscard]] bool ReadManagedStructuredSetting(scene::IScriptSerializable& value, OpData& output,
    std::vector<scene::ScriptObjectReference>* references = nullptr);
using ManagedStructuredReferencePaths = std::unordered_map<std::string, scene::ScriptObjectReference>;
/// @note RFC 6901 の path で寿命検証済み投影を索引化する。UI の表示順・折りたたみ状態に依存しない。
[[nodiscard]] std::string ManagedSettingChildPath(std::string_view parent, std::string_view key);
[[nodiscard]] bool IndexManagedStructuredReferences(const OpData& value,
    const std::vector<scene::ScriptObjectReference>& references, ManagedStructuredReferencePaths& output);
} /// @note namespace fbzz::editor
