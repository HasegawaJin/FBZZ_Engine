/// @file    ManagedScriptPublicationState.hpp
/// @brief   コンパイル完了と Play/Stop のソース検証から Edit 適用可否を決める。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once

#include <Editor/ManagedScriptEditor.hpp>
#include <cassert>
#include <cstdint>

namespace fbzz::editor {

enum class ManagedStopVerification { WAITING, UNCHANGED, CHANGED };

/// @pre watcher の ticket と snapshot は同じ監視セッションに属し、メインスレッドで扱う。
/// @see Docs/design/csharp-scripting.md コンパイル完了と Edit 適用の内部境界。
class ManagedScriptPublicationState final {
public:
    void BeginPlay(const ManagedSourceSnapshot& source)
    {
        if (m_stopVerificationRevision != 0) m_explicitEditApplyRequired = true;
        m_parkedSourceFingerprint = source.fingerprint;
        m_parkedSourceVerified = source.valid;
    }

    /// @note コンパイラー完了後も新しいソース検証が終わるまで、新コードを進行中として扱う。
    void EndPlay(bool newerAssembly, bool otherWorkPending, std::uint64_t stopRevision)
    {
        m_explicitEditApplyRequired = m_explicitEditApplyRequired || newerAssembly || otherWorkPending ||
            m_completionVerificationRevision != 0;
        m_stopVerificationRevision = stopRevision;
    }

    /// @return 古い・無効な snapshot は WAITING。新しい snapshot だけ Stop ticket を消費する。
    [[nodiscard]] ManagedStopVerification VerifyStoppedSource(const ManagedSourceSnapshot& source)
    {
        if (m_stopVerificationRevision == 0 || !source.valid || source.revision < m_stopVerificationRevision)
            return ManagedStopVerification::WAITING;
        m_stopVerificationRevision = 0;
        if (!m_parkedSourceVerified || source.fingerprint != m_parkedSourceFingerprint) {
            m_explicitEditApplyRequired = true;
            return ManagedStopVerification::CHANGED;
        }
        return ManagedStopVerification::UNCHANGED;
    }

    /// @return 解除前に明示適用が必要だったか。後続のビルド・metadata 検証は呼び出し側が行う。
    [[nodiscard]] bool RequestExplicitApply()
    {
        const bool required = m_explicitEditApplyRequired;
        m_explicitEditApplyRequired = false;
        m_stopVerificationRevision = 0;
        return required;
    }

    /// @pre 実コンパイラーの完了後に watcher が発行した非 0 ticket を渡す。
    void AwaitCompileVerification(std::uint64_t revision)
    {
        assert(revision != 0 && m_completionVerificationRevision == 0);
        m_completionVerificationRevision = revision;
    }

    /// @note 内容の valid 判定は消費後にサービスが行い、無効な入力は通常の再試行へ渡す。
    [[nodiscard]] bool TryCompleteCompileVerification(std::uint64_t revision)
    {
        if (m_completionVerificationRevision == 0 || revision < m_completionVerificationRevision) return false;
        m_completionVerificationRevision = 0;
        return true;
    }

    [[nodiscard]] bool HasCompileVerification() const { return m_completionVerificationRevision != 0; }
    [[nodiscard]] bool HasStopVerification() const { return m_stopVerificationRevision != 0; }
    [[nodiscard]] bool HasPendingVerification() const { return HasCompileVerification() || HasStopVerification(); }
    [[nodiscard]] bool RequiresExplicitEditApply() const { return m_explicitEditApplyRequired; }
    [[nodiscard]] bool CanAutomaticallyApply(bool inEditor) const
    {
        return !inEditor || (!m_explicitEditApplyRequired && !HasStopVerification());
    }

    void Reset()
    {
        m_parkedSourceFingerprint = 0;
        m_parkedSourceVerified = false;
        m_stopVerificationRevision = 0;
        m_completionVerificationRevision = 0;
        m_explicitEditApplyRequired = false;
    }

private:
    std::uint64_t m_parkedSourceFingerprint = 0;
    std::uint64_t m_stopVerificationRevision = 0;
    std::uint64_t m_completionVerificationRevision = 0;
    bool m_parkedSourceVerified = false;
    bool m_explicitEditApplyRequired = false;
};

} /// @note namespace fbzz::editor
