/// @file    MaterialAuthoringState.hpp
/// @brief   Owned material drafts publish deeply immutable authoring values.
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

/// @note A conflict leaves the current owner unchanged; callers must preserve their existing failure/notification contract.
enum class MaterialAuthoringCommitResult : uint8_t { COMMITTED, UNCHANGED, CONFLICT };

/// @note Signed zero and distinct NaN payloads remain authored changes; approximate math equality cannot witness an unchanged publication.
[[nodiscard]] inline bool MaterialAuthoringFloatEqual(float lhs, float rhs)
{
    return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs);
}

[[nodiscard]] inline bool MaterialAuthoringFloatMapsEqual(
    const std::unordered_map<std::string, std::vector<float>>& lhs,
    const std::unordered_map<std::string, std::vector<float>>& rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (const auto& [key, values] : lhs) {
        const auto found = rhs.find(key);
        if (found == rhs.end() || values.size() != found->second.size()) return false;
        for (size_t index = 0; index < values.size(); ++index)
            if (!MaterialAuthoringFloatEqual(values[index], found->second[index])) return false;
    }
    return true;
}

/// @note Values must own their strings/containers and contain no borrowed mutable authoring aliases.
/// @note Access is confined to the existing scene/asset authoring thread; this does not make those owners concurrently writable.
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
        /// @note CreationTag is private and constructible only by this state; allocator forwarding requires this constructor to be public.
        Snapshot(CreationTag, std::shared_ptr<const OwnerIdentity> owner,
            uint64_t revision, const Values& values)
            : m_owner(std::move(owner)), m_revision(revision), m_values(values) {}

        [[nodiscard]] const Values& Data() const { return m_values; }
        [[nodiscard]] uint64_t Revision() const { return m_revision; }
        [[nodiscard]] const void* OwnerToken() const { return m_owner.get(); }

    private:
        std::shared_ptr<const OwnerIdentity> m_owner;
        uint64_t m_revision = 1;
        Values m_values;
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

        /// @note The live target must be reacquired before calling; the edit retains no raw owner pointer.
        template<typename Owner>
        [[nodiscard]] MaterialAuthoringCommitResult Commit(Owner& target)
        {
            return target.CommitAuthoringEdit(*this);
        }
        void Cancel() { m_terminal = true; }

    private:
        explicit Edit(SnapshotPtr base) : m_base(std::move(base)),
            m_draft(m_base ? m_base->Data() : Values{}) {}
        SnapshotPtr m_base;
        Values m_draft;
        bool m_terminal = false;
        friend class MaterialAuthoringState;
    };

    MaterialAuthoringState()
        : m_snapshot(MakeInitial(Values{})) {}
    explicit MaterialAuthoringState(const Values& values)
        : m_snapshot(MakeInitial(values)) {}
    /// @note Copying values creates a distinct authoring owner, even when the source snapshot is retained elsewhere.
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
    /// @note Move transfers the owner identity; the moved-from state has no cacheable snapshot until an edit/replacement initializes it.
    MaterialAuthoringState(MaterialAuthoringState&&) noexcept = default;
    MaterialAuthoringState& operator=(MaterialAuthoringState&&) noexcept = default;

    [[nodiscard]] SnapshotPtr Read() const { return m_snapshot; }
    /// @pre The borrowed view must not cross a mutation, reload or callback; use Read() for an owned lifetime.
    [[nodiscard]] const Snapshot* View() const { return m_snapshot.get(); }
    [[nodiscard]] Edit BeginEdit()
    {
        if (!m_snapshot) m_snapshot = MakeInitial(Values{});
        return Edit(m_snapshot);
    }

    /// @note Publication always copies the draft. References retained from Edit::Draft cannot mutate the resulting const snapshot.
    /// @pre The owner remains alive for this member call; standard value copies/allocators must not destroy the owner.
    [[nodiscard]] MaterialAuthoringCommitResult Commit(Edit& edit)
    {
        if (edit.m_terminal || !edit.m_base || m_snapshot != edit.m_base)
            return MaterialAuthoringCommitResult::CONFLICT;
        edit.m_terminal = true;
        if constexpr (requires(const Values& lhs, const Values& rhs) { lhs == rhs; }) {
            if (m_snapshot->Data() == edit.m_draft)
                return MaterialAuthoringCommitResult::UNCHANGED;
        }
        const auto base = edit.m_base;
        auto next = Prepare(base, edit.m_draft);
        if (m_snapshot != base)
            return MaterialAuthoringCommitResult::CONFLICT;
        m_snapshot = std::move(next);
        return MaterialAuthoringCommitResult::COMMITTED;
    }

    /// @note Replacement is an explicit new publication, used by intentional whole-value assignment/load/undo.
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
        if (!base || base->Revision() == (std::numeric_limits<uint64_t>::max)())
            return MakeInitial(values);
        return std::make_shared<const Snapshot>(CreationTag{}, base->m_owner, base->Revision() + 1u, values);
    }
    static SnapshotPtr MakeInitial(const Values& values)
    {
        auto owner = std::make_shared<const OwnerIdentity>();
        return std::make_shared<const Snapshot>(CreationTag{}, std::move(owner), 1u, values);
    }
    SnapshotPtr m_snapshot;
};

} /// @note namespace fbzz::asset::detail
