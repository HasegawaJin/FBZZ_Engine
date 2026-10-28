/// @file    ManagedScriptEditor.hpp
/// @brief   編集時の C# ビルドと適用済み世代を管理する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene { class Scene; }
namespace fbzz::editor {

class Compiler;
class OperatorRegistry;
class ManagedScriptPublicationState;

struct ManagedSourceSnapshot {
    std::uint64_t revision = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t withoutMetadataFingerprint = 0;
    std::uint64_t rootInputFingerprint = 0;
    bool hasInputManifest = false;
    bool valid = false;
};

/// @note 実装は読み取りだけを worker で行う。要求 ticket より前に始まった snapshot を検証に使わない。
class IManagedSourceWatcher {
public:
    virtual ~IManagedSourceWatcher() = default;
    virtual void Start(const std::filesystem::path& project, const std::filesystem::path& inputManifest) = 0;
    virtual void Stop() = 0;
    /// @return 同じ監視セッション内で増加する非 0 ticket。要求より前の snapshot と区別する。
    [[nodiscard]] virtual std::uint64_t RequestRefresh() = 0;
    /// @note main thread は worker を待たない。結果を即時取得できなければ false を返し、次フレームへ延期する。
    [[nodiscard]] virtual bool Poll(ManagedSourceSnapshot& snapshot) = 0;
};

/// @note MSBuild の評価済み入力と各プロジェクトのソースを内容で照合し、日時を保持した変更も検出する。
/// @return 読み取り失敗なら false。削除された依存入力は欠損を版へ含める。
[[nodiscard]] bool ComputeManagedSourceFingerprint(const std::filesystem::path& project,
    const std::filesystem::path& inputManifest, std::uint64_t& fingerprint, bool includeMetadata = true);
/// @note 入力内容の読み取りを一回にまとめ、root と full graph の版を同じ byte stream から計算する。通常は worker、同期呼出は検証用。
[[nodiscard]] bool ComputeManagedSourceSnapshot(const std::filesystem::path& project,
    const std::filesystem::path& inputManifest, ManagedSourceSnapshot& snapshot);

enum class ManagedBuildState {
    SCANNING, QUEUED, COMPILING, VALIDATING, COMPILED, APPLYING, APPLIED, FAILED, DEFERRED, SUPERSEDED
};

struct ManagedBuildDiagnostic {
    bool warning = false;
    std::string file;
    int line = 0;
    int column = 0;
    std::string code;
    std::string message;
};

struct ManagedBuildRequest {
    std::string managedRequestId;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t effectiveSourceFingerprint = 0;
    std::uint64_t previousGeneration = 0;
    std::uint64_t appliedGeneration = 0;
    ManagedBuildState state = ManagedBuildState::QUEUED;
    bool automatic = false;
    unsigned inputDiscoveryRetries = 0;
    int exitCode = 0;
    std::string candidateId;
    std::string errorCode;
    std::string message;
    std::string log;
    std::string logFilePath;
    std::uint64_t logOriginalBytes = 0;
    std::uint64_t logRetainedBytes = 0;
    std::vector<ManagedBuildDiagnostic> diagnostics;
    double elapsedMilliseconds = 0.0;
    double queueMilliseconds = 0.0;
    double compileMilliseconds = 0.0;
    double applyMilliseconds = 0.0;
    double sourceMilliseconds = 0.0;
    double validationMilliseconds = 0.0;
    std::chrono::steady_clock::time_point queuedAt{};
    std::chrono::steady_clock::time_point stageStartedAt{};
};

/// @note 要求 ID は Editor プロセス内で一意。履歴の外へ出た要求を最新要求へ読み替えない。
class ManagedBuildHistory final {
public:
    static constexpr std::size_t MAX_HISTORY = 32;
    /// @note 同じ版の進行中要求へ合流し、異なる版の進行中要求は superseded で終了する。
    [[nodiscard]] std::string Queue(std::uint64_t fingerprint, bool automatic, std::uint64_t previousGeneration);
    void BeginCompile(std::string_view requestId, std::string candidateId);
    /// @return 現在のソース版に一致するコンパイル成功だけ true。
    bool CompleteCompile(std::string_view requestId, std::uint64_t currentFingerprint,
        bool success, bool assemblyExists, int exitCode, const std::string& log);
    /// @note 初回 graph 発見だけを同じ要求で再試行する。最初の成果物は適用せず、全 graph を捕捉して再コンパイルする。
    [[nodiscard]] bool CanRetryDiscoveredGraph(std::string_view requestId, const ManagedSourceSnapshot& captured,
        const ManagedSourceSnapshot& current) const;
    bool RetryDiscoveredGraph(std::string_view requestId, const ManagedSourceSnapshot& captured,
        const ManagedSourceSnapshot& current, bool success, bool assemblyExists, int exitCode, const std::string& log);
    /// @note 終端状態を後続の古いコンパイラー応答で上書きしない。
    void Transition(std::string_view requestId, ManagedBuildState state,
        std::string errorCode = {}, std::string message = {});
    /// @note importer 自身の .cs.meta 生成は要求版を置換せず、有効版だけを更新する。
    void SetEffectiveFingerprint(std::string_view requestId, std::uint64_t fingerprint);
    /// @note 非同期の最初の snapshot が完了した時だけ、scanning 要求の版を固定する。
    void BindSourceFingerprint(std::string_view requestId, std::uint64_t fingerprint);
    void MarkApplied(std::string_view requestId, std::uint64_t generation);
    [[nodiscard]] const ManagedBuildRequest* Find(std::string_view requestId) const;
    [[nodiscard]] const ManagedBuildRequest* Latest() const;
    [[nodiscard]] const std::deque<ManagedBuildRequest>& History() const { return m_history; }
    [[nodiscard]] static bool IsTerminal(ManagedBuildState state);
    [[nodiscard]] static const char* StateName(ManagedBuildState state);
    /// @note 進行中の段階を含む経過時間のコピーを返し、履歴自体は変更しない。
    [[nodiscard]] static ManagedBuildRequest WithCurrentTiming(const ManagedBuildRequest& request);

private:
    [[nodiscard]] ManagedBuildRequest* FindMutable(std::string_view requestId);
    std::deque<ManagedBuildRequest> m_history;
};

class ManagedScriptEditor final {
public:
    explicit ManagedScriptEditor(std::unique_ptr<IManagedSourceWatcher> watcher = {});
    ~ManagedScriptEditor();
    ManagedScriptEditor(const ManagedScriptEditor&) = delete;
    ManagedScriptEditor& operator=(const ManagedScriptEditor&) = delete;

    /// @note 起動Editorに対応するSDK/repositoryのTools・Host・APIを一緒に解決し、配布Editorは親repositoryへfallbackしない。
    void Init(const std::filesystem::path& projectRoot, const std::filesystem::path& engineRoot);
    /// @pre メインスレッドの Script callback 外で呼ぶ。
    /// @note Edit / Play の callback 後の安全点で適用する。
    void Tick(scene::Scene* scene, bool inEditor, float deltaSeconds, bool otherBuildRunning);
    /// @pre Scene を破棄する前に呼ぶ。
    void Shutdown(scene::Scene* scene);
    /// @pre メインスレッドから呼ぶ。worker の次の内容 snapshot を要求し、scanning 完了時に版を固定する。
    /// @return 利用不可なら false。requestId は未変更。
    [[nodiscard]] bool RequestBuild(std::string& requestId);
    void BeginPlaySession();
    /// @note Restores active Edit generation status; pending newer code requires an explicit build/apply request after Stop.
    void EndPlaySession();
    [[nodiscard]] bool RequiresExplicitEditApply() const;
    [[nodiscard]] bool IsAvailable() const { return m_ready; }
    [[nodiscard]] bool IsBusy() const;
    [[nodiscard]] const std::string& AvailabilityDiagnostic() const { return m_availabilityDiagnostic; }
    [[nodiscard]] const ManagedBuildHistory& Builds() const { return m_builds; }
    [[nodiscard]] std::uint64_t AppliedGeneration() const { return m_appliedGeneration; }
    static void RegisterOperator(OperatorRegistry& registry);

private:
    void StartBuild();
    void ApplyPending(scene::Scene* scene);
    void LoadPrevious();
    void QueueBuild(std::uint64_t fingerprint, bool automatic, float debounceSeconds);
    void RejectPending(const char* code, const std::string& diagnostic);
    void CleanupOwnedGenerations();
    /// @note 通常フレームは完了済み結果だけを回収する。project 切替と終了だけ worker 完了を待つ。
    void CompleteGenerationCleanup(bool wait);
    void SaveOwnedGenerations();

    std::unique_ptr<Compiler> m_compiler;
    std::unique_ptr<IManagedSourceWatcher> m_sourceWatcher;
    std::unique_ptr<ManagedScriptPublicationState> m_publication;
    ManagedSourceSnapshot m_sourceSnapshot;
    ManagedSourceSnapshot m_compileSnapshot;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_engineRoot;
    std::filesystem::path m_project;
    std::filesystem::path m_agentBuild;
    std::filesystem::path m_hostDirectory;
    std::string m_configuration;
    std::filesystem::path m_outputRoot;
    std::filesystem::path m_buildOutput;
    std::filesystem::path m_pendingAssembly;
    std::filesystem::path m_appliedAssemblyDirectory;
    std::filesystem::path m_previousAssemblyDirectory;
    std::filesystem::path m_parkedAssemblyDirectory;
    std::filesystem::path m_latestPlayAssemblyDirectory;
    bool m_playSession = false;
    uint64_t m_parkedAppliedGeneration = 0;
    std::vector<std::filesystem::path> m_ownedGenerations;
    std::future<std::vector<std::filesystem::path>> m_generationCleanup;
    std::future<bool> m_generationOwnershipSave;
    std::uint64_t m_observedFingerprint = 0;
    std::uint64_t m_buildFingerprint = 0;
    std::uint64_t m_nextBuild = 0;
    std::uint64_t m_appliedGeneration = 0;
    ManagedBuildHistory m_builds;
    std::string m_queuedRequestId;
    std::string m_compilingRequestId;
    std::string m_pendingRequestId;
    std::string m_availabilityDiagnostic = "C# build service is not initialized";
    std::string m_sourceRequestId;
    std::uint64_t m_sourceRequestRevision = 0;
    std::uint64_t m_startVerificationRevision = 0;
    std::uint64_t m_metadataVerificationRevision = 0;
    std::uint64_t m_metadataSourceFingerprint = 0;
    float m_debounceSeconds = 0.0f;
    float m_applyRetrySeconds = 0.0f;
    float m_discoverySeconds = 0.0f;
    float m_cleanupSeconds = 0.0f;
    bool m_ready = false;
    bool m_buildPending = false;
    bool m_collectionWaiting = false;
    bool m_manifestPrepared = false;
    bool m_initialSourcePending = false;
};

} /// @note namespace fbzz::editor
