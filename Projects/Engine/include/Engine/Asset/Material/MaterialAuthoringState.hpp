/// @file    MaterialAuthoringState.hpp
/// @brief   所有する材質の下書きを、参照先も含めて不変な編集値として公開する。
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <cstdint>
#include <cstddef>
#include <bit>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::asset::detail {

/// @note 競合時は現在の所有者の値を変更しない。呼び出し側は既存の失敗処理と通知の契約に従う。
enum class MaterialAuthoringCommitResult : uint8_t { COMMITTED, UNCHANGED, CONFLICT };

/// @note 符号付きゼロの違いと NaN ペイロードの違いも編集とみなすため、近似比較では公開値が不変か判定できない。
[[nodiscard]] inline bool MaterialAuthoringFloatEqual(float lhs, float rhs)
{
    return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs);
}

[[nodiscard]] inline bool MaterialAuthoringFloatMapsEqual(
    const std::unordered_map<std::string, std::vector<float>>& lhs,
    const std::unordered_map<std::string, std::vector<float>>& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (const auto& [key, values] : lhs) {
        const auto found = rhs.find(key);
        if (found == rhs.end() || values.size() != found->second.size()) {
            return false;
        }
        for (size_t index = 0; index < values.size(); ++index)
            if (!MaterialAuthoringFloatEqual(values[index], found->second[index])) {
                return false;
            }
    }
    return true;
}

/// @note 値は文字列とコンテナを所有し、変更可能な外部の編集値を参照しないこと。
/// @note 既存のシーン・アセット編集スレッドからのみアクセスする。同じ所有者への並行書き込みは許可しない。
template<typename Values>
class MaterialAuthoringState {
private:
    struct OwnerIdentity {};
    struct CreationTag {
        CreationTag(const CreationTag&) = default;
    private:
        CreationTag() = default;
        friend class MaterialAuthoringState;
    };

public:
    class Snapshot {
    public:
        /// @note CreationTag はこの状態だけが生成できる非公開の型。アロケータが引数を転送するため、コンストラクタは公開する。
        Snapshot(CreationTag, std::shared_ptr<const OwnerIdentity> owner,
            uint64_t revision, const Values& values)
            : m_owner(std::move(owner)), m_revision(revision), m_values(values) {}

        [[nodiscard]] const Values& Data() const { return m_values; }
        [[nodiscard]] uint64_t Revision() const { return m_revision; }
        [[nodiscard]] const void* OwnerToken() const { return m_owner.get(); }

    private:
        std::shared_ptr<const OwnerIdentity> m_owner; ///< スナップショットが由来する制作状態を照合する所有者識別子。
        uint64_t m_revision = 1; ///< この制作値が公開された版番号。
        Values m_values; ///< この版で公開された不変の制作値。
        friend class MaterialAuthoringState;
    };
    using SnapshotPtr = std::shared_ptr<const Snapshot>;

    class Edit {
    public:
        Edit(const Edit&) = delete;
        Edit& operator=(const Edit&) = delete;
        Edit(Edit&&) = default;
        Edit& operator=(Edit&&) = default;
        [[nodiscard]] Values& Draft() { return m_draft; }
        [[nodiscard]] const Values& Draft() const { return m_draft; }

        /// @note 編集は所有者の生ポインターを保持しないため、呼び出し前に現行の対象を取得し直す。
        template<typename Owner>
        [[nodiscard]] MaterialAuthoringCommitResult Commit(Owner& target)
        {
            return target.CommitAuthoringEdit(*this);
        }
        void Cancel() { m_terminal = true; }

    private:
        explicit Edit(SnapshotPtr base) : m_base(std::move(base)),
            m_draft(m_base ? m_base->Data() : Values{}) {}
        SnapshotPtr m_base; ///< 編集開始時の制作値。確定時に競合を照合するため保持する。
        Values m_draft; ///< 確定前の変更を蓄える編集用の値のコピー。
        bool m_terminal = false; ///< 編集が確定または取り消され、操作を終えたか。
        friend class MaterialAuthoringState;
    };

    MaterialAuthoringState()
        : m_snapshot(MakeInitial(Values{})) {}
    explicit MaterialAuthoringState(const Values& values)
        : m_snapshot(MakeInitial(values)) {}
    /// @note 元のスナップショットを別の場所で保持していても、値をコピーした側は別の編集所有者となる。
    MaterialAuthoringState(const MaterialAuthoringState& other)
        : m_snapshot(CopyInitial(other.m_snapshot)) {}
    MaterialAuthoringState& operator=(const MaterialAuthoringState& other)
    {
        if (this != &other) {
            const auto source = other.m_snapshot;
            m_snapshot = CopyInitial(source);
        }
        return *this;
    }
    /// @note ムーブで所有者の識別情報を移す。ムーブ元は編集または置換で初期化するまでキャッシュ可能なスナップショットを持たない。
    MaterialAuthoringState(MaterialAuthoringState&&) noexcept = default;
    MaterialAuthoringState& operator=(MaterialAuthoringState&&) noexcept = default;

    [[nodiscard]] SnapshotPtr Read() const { return m_snapshot; }
    /// @pre 借用したビューは変更・再読込・コールバックをまたいで使わない。寿命を延ばす場合は Read() で所有する。
    [[nodiscard]] const Snapshot* View() const { return m_snapshot.get(); }
    [[nodiscard]] Edit BeginEdit()
    {
        if (!m_snapshot) {
            m_snapshot = MakeInitial(Values{});
        }
        return Edit(m_snapshot);
    }

    /// @note 公開時は下書きを必ずコピーするため、Edit::Draft から得た参照を保持しても公開後の const 値は変更できない。
    /// @pre メンバー呼び出し中は所有者が生存していること。通常の値コピーやアロケータから所有者を破棄しないこと。
    [[nodiscard]] MaterialAuthoringCommitResult Commit(Edit& edit)
    {
        if (edit.m_terminal || !edit.m_base || m_snapshot != edit.m_base) {
            return MaterialAuthoringCommitResult::CONFLICT;
        }
        edit.m_terminal = true;
        if constexpr (requires(const Values& lhs, const Values& rhs) { lhs == rhs; }) {
            if (m_snapshot->Data() == edit.m_draft) {
                return MaterialAuthoringCommitResult::UNCHANGED;
            }
        }
        const auto base = edit.m_base;
        auto next = Prepare(base, edit.m_draft);
        if (m_snapshot != base) {
            return MaterialAuthoringCommitResult::CONFLICT;
        }
        m_snapshot = std::move(next);
        return MaterialAuthoringCommitResult::COMMITTED;
    }

    /// @note 置換は明示的な新規公開で、意図的な全値代入・読込・取り消しに使う。
    void Replace(const Values& values)
    {
        const auto base = m_snapshot;
        auto next = Prepare(base, values);
        m_snapshot = std::move(next);
    }

private:
    static SnapshotPtr CopyInitial(SnapshotPtr source)
    {
        return MakeInitial(source ? source->Data() : Values{});
    }
    static SnapshotPtr Prepare(const SnapshotPtr& base, const Values& values)
    {
        if (!base || base->Revision() == (std::numeric_limits<uint64_t>::max)()) {
            return MakeInitial(values);
        }
        return std::make_shared<const Snapshot>(CreationTag{}, base->m_owner, base->Revision() + 1u, values);
    }
    static SnapshotPtr MakeInitial(const Values& values)
    {
        auto owner = std::make_shared<const OwnerIdentity>();
        return std::make_shared<const Snapshot>(CreationTag{}, std::move(owner), 1u, values);
    }
    SnapshotPtr m_snapshot; ///< 現在公開中の不変制作値を保持する共有参照。
};

} // namespace fbzz::asset::detail
