/// @file    ManagedScriptEditor.cpp
/// @brief   C# の変更を独立したビルドへ送り、Edit の安全点で適用する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Editor/ManagedScriptEditor.hpp>
#include <Editor/Util/ManagedSdkBinding.hpp>
#include <Editor/PackageBuildService.hpp>
#include "ManagedScriptMetadata.hpp"
#include "ManagedScriptPublicationState.hpp"
#include <Editor/Compiler.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <regex>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fbzz::editor {
namespace {

bool IsAssembly(const std::filesystem::path& directory, const std::filesystem::path& project)
{
    std::error_code error;
    return std::filesystem::is_regular_file(directory / (project.stem().wstring() + L".dll"), error) && !error;
}

bool IsManagedSource(const std::filesystem::path& path, bool includeMetadata)
{
    std::string name = util::FileSystem::PathToUtf8(path.filename());
    for (char& character : name)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return name.ends_with(".cs") || (includeMetadata && name.ends_with(".cs.meta"));
}

bool IsManagedConfiguration(const std::filesystem::path& path)
{
    std::string name = util::FileSystem::PathToUtf8(path.filename());
    for (char& value : name) if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
    return name.ends_with(".csproj") || name.ends_with(".props") || name.ends_with(".targets") ||
        name == "global.json" || name == "nuget.config" || name == ".editorconfig";
}

bool SkipManagedDirectory(const std::filesystem::path& path)
{
    std::string name = util::FileSystem::PathToUtf8(path.filename());
    for (char& value : name) if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
    return name == "obj" || name == "bin" || name == "library" || name == "build" ||
        name == ".git" || name == ".vs" || name == "scratch";
}

/// @note Windows junction も含め、監視による再帰探索は reparse point をたどらない。
bool IsManagedReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsManagedCacheScope(std::wstring_view scope)
{
    if (scope.empty() || scope.size() > 64) return false;
    std::wstring upper;
    upper.reserve(scope.size());
    for (const wchar_t value : scope) {
        const bool letter = (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z');
        if (!letter && !(value >= L'0' && value <= L'9') && value != L'-' && value != L'_') return false;
        upper.push_back(value >= L'a' && value <= L'z' ? static_cast<wchar_t>(value - L'a' + L'A') : value);
    }
    if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL"
        || upper.starts_with(L"GENERATION-")) return false;
    return !(upper.size() == 4 && (upper.starts_with(L"COM") || upper.starts_with(L"LPT"))
        && upper[3] >= L'1' && upper[3] <= L'9');
}

bool IsOwnedManagedGenerationName(std::string name)
{
    if (name.ends_with(".pending")) name.resize(name.size() - 8);
    if (!name.starts_with("generation-")) return false;
    const std::string suffix = name.substr(11);
    const std::size_t separator = suffix.find('-');
    return separator != std::string::npos && separator != 0 && separator + 1 < suffix.size() &&
        std::count(suffix.begin(), suffix.end(), '-') == 1 &&
        std::all_of(suffix.begin(), suffix.end(), [](unsigned char value) { return (value >= '0' && value <= '9') || value == '-'; });
}

std::string OwnedGenerationLedger(const std::vector<std::filesystem::path>& paths)
{
    std::string text;
    for (const auto& path : paths) text += util::FileSystem::PathToUtf8(path) + '\n';
    return text;
}

std::vector<ManagedBuildDiagnostic> ParseManagedDiagnostics(const std::string& log)
{
    const std::regex diagnosticPattern(R"(^(ERROR|WARN) (.+?) ([A-Z][A-Z0-9-]*) (.*)$)");
    const std::regex locationPattern(R"(^(.+?):(\d+)(?::(\d+))?$)");
    std::vector<ManagedBuildDiagnostic> diagnostics;
    std::size_t offset = 0;
    while (offset < log.size()) {
        const std::size_t end = log.find('\n', offset);
        std::string line = log.substr(offset, end == std::string::npos ? end : end - offset);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        if (std::regex_match(line, match, diagnosticPattern)) {
            ManagedBuildDiagnostic diagnostic;
            diagnostic.warning = match[1].str() == "WARN";
            diagnostic.file = match[2].str();
            diagnostic.code = match[3].str();
            diagnostic.message = match[4].str();
            std::smatch location;
            if (std::regex_match(diagnostic.file, location, locationPattern)) {
                const auto readNumber = [](const std::string& text) {
                    int value = 0;
                    for (const char digit : text) {
                        if (value > 100000000) return 0;
                        value = value * 10 + digit - '0';
                    }
                    return value;
                };
                const std::string file = location[1].str();
                diagnostic.line = readNumber(location[2].str());
                diagnostic.column = location[3].matched ? readNumber(location[3].str()) : 0;
                diagnostic.file = file;
            }
            diagnostics.push_back(std::move(diagnostic));
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return diagnostics;
}

std::string ParseManagedBuildLogPath(const std::string& log)
{
    std::string result;
    std::size_t offset = 0;
    while (offset < log.size()) {
        const std::size_t end = log.find('\n', offset);
        std::string_view line(log.data() + offset, (end == std::string::npos ? log.size() : end) - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t marker = line.rfind(" log=");
        if (marker != std::string_view::npos && line.starts_with("RESULT ")) result = std::string(line.substr(marker + 5));
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return result;
}

} /// @note namespace

static bool ComputeManagedSourceFingerprintImpl(const std::filesystem::path& project,
    const std::filesystem::path& inputManifest, std::uint64_t& fingerprint, bool includeMetadata,
    std::uint64_t* withoutMetadata = nullptr, std::stop_token cancellation = {},
    std::uint64_t* rootInputFingerprint = nullptr, bool* hasInputManifest = nullptr)
{
    std::error_code error;
    std::vector<std::filesystem::path> projects{ project };
    std::vector<std::filesystem::path> importDirectories;
    std::vector<std::filesystem::path> recursiveImportDirectories;
    std::vector<std::filesystem::path> sources{ project };
    std::vector<std::filesystem::path> rootInputs{ project };
    const bool manifestExists = std::filesystem::exists(inputManifest, error);
    if (manifestExists) {
        std::ifstream manifest(inputManifest, std::ios::binary);
        if (!manifest) return false;
        std::string line;
        while (std::getline(manifest, line)) {
            if (cancellation.stop_requested()) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() < 3 || line[1] != '|' || (line[0] != 'F' && line[0] != 'P' && line[0] != 'D' && line[0] != 'R')) return false;
            const std::filesystem::path path = std::filesystem::u8path(line.substr(2));
            if (!path.is_absolute()) return false;
            if (line[0] == 'P') projects.push_back(path);
            else if (line[0] == 'D') importDirectories.push_back(path);
            else if (line[0] == 'R') recursiveImportDirectories.push_back(path);
            else if (includeMetadata || !util::FileSystem::PathToUtf8(path.filename()).ends_with(".cs.meta"))
                sources.push_back(path);
        }
        if (!manifest.eof()) return false;
    }
    if (error) return false;
    std::sort(projects.begin(), projects.end());
    projects.erase(std::unique(projects.begin(), projects.end()), projects.end());
    for (const std::filesystem::path& dependency : projects) {
        if (cancellation.stop_requested()) return false;
        sources.push_back(dependency);
        /// @note 新しく追加された上位の暗黙 import も、前回 manifest の有無に依存せず検出する。
        for (std::filesystem::path parent = dependency.parent_path(); !parent.empty();) {
            for (const char* name : { "Directory.Build.props", "Directory.Build.targets", "Directory.Packages.props", "global.json", "NuGet.Config", ".editorconfig" }) {
                sources.push_back(parent / name);
                if (dependency == project) rootInputs.push_back(parent / name);
            }
            const std::filesystem::path next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
        if (!std::filesystem::exists(dependency.parent_path(), error)) {
            if (error) return false;
            continue;
        }
        if (IsManagedReparsePoint(dependency.parent_path())) continue;
        std::filesystem::recursive_directory_iterator iterator(dependency.parent_path(), error);
        if (error) return false;
        for (; iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error)) {
            if (cancellation.stop_requested()) return false;
            if (error) return false;
            if (IsManagedReparsePoint(iterator->path())) {
                iterator.disable_recursion_pending();
                continue;
            }
            if (error) return false;
            if (iterator->is_directory(error) && SkipManagedDirectory(iterator->path())) {
                iterator.disable_recursion_pending();
                continue;
            }
            if (iterator->is_regular_file(error) &&
                (IsManagedSource(iterator->path(), includeMetadata) || IsManagedConfiguration(iterator->path())))
            {
                sources.push_back(iterator->path());
                if (dependency == project) rootInputs.push_back(iterator->path());
            }
            if (error) return false;
        }
        if (error) return false;
    }
    std::sort(importDirectories.begin(), importDirectories.end());
    importDirectories.erase(std::unique(importDirectories.begin(), importDirectories.end()), importDirectories.end());
    for (const std::filesystem::path& directory : importDirectories) {
        if (cancellation.stop_requested()) return false;
        sources.push_back(directory);
        if (!std::filesystem::exists(directory, error)) {
            if (error) return false;
            continue;
        }
        if (IsManagedReparsePoint(directory)) continue;
        std::filesystem::directory_iterator iterator(directory, error);
        if (error) return false;
        for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
            if (cancellation.stop_requested()) return false;
            if (error) return false;
            if (!IsManagedReparsePoint(iterator->path()) && iterator->is_regular_file(error) && IsManagedConfiguration(iterator->path()))
                sources.push_back(iterator->path());
            if (error) return false;
        }
        if (error) return false;
    }
    for (const std::filesystem::path& directory : recursiveImportDirectories) {
        if (cancellation.stop_requested()) return false;
        sources.push_back(directory);
        if (!std::filesystem::exists(directory, error)) {
            if (error) return false;
            continue;
        }
        if (IsManagedReparsePoint(directory)) continue;
        std::filesystem::recursive_directory_iterator iterator(directory, error);
        if (error) return false;
        for (; iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error)) {
            if (cancellation.stop_requested()) return false;
            if (error) return false;
            if (IsManagedReparsePoint(iterator->path()) || (iterator->is_directory(error) && SkipManagedDirectory(iterator->path()))) {
                iterator.disable_recursion_pending();
                continue;
            }
            if (iterator->is_regular_file(error) && IsManagedConfiguration(iterator->path())) sources.push_back(iterator->path());
            if (error) return false;
        }
        if (error) return false;
    }
    for (std::filesystem::path& source : sources) {
        source = std::filesystem::absolute(source, error).lexically_normal();
        if (error) return false;
    }
    for (auto& source : rootInputs) {
        source = std::filesystem::absolute(source, error).lexically_normal();
        if (error) return false;
    }
    std::sort(rootInputs.begin(), rootInputs.end());
    rootInputs.erase(std::unique(rootInputs.begin(), rootInputs.end()), rootInputs.end());
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    std::uint64_t hash = 14695981039346656037ULL;
    std::uint64_t sourceHash = hash;
    std::uint64_t rootHash = hash;
    bool metadata = false;
    bool rootInput = false;
    const auto append = [&hash, &sourceHash, &metadata, &rootHash, &rootInput](unsigned char value) {
        hash = (hash ^ value) * 1099511628211ULL;
        if (!metadata) sourceHash = (sourceHash ^ value) * 1099511628211ULL;
        if (rootInput) rootHash = (rootHash ^ value) * 1099511628211ULL;
    };
    std::array<char, 8192> buffer{};
    for (const std::filesystem::path& source : sources) {
        if (cancellation.stop_requested()) return false;
        metadata = util::FileSystem::PathToUtf8(source.filename()).ends_with(".cs.meta");
        rootInput = std::binary_search(rootInputs.begin(), rootInputs.end(), source);
        for (const unsigned char value : util::FileSystem::PathToUtf8(source)) append(value);
        append(0);
        const bool exists = std::filesystem::exists(source, error);
        if (error) return false;
        append(exists ? 1 : 0);
        if (!exists) continue;
        if (std::filesystem::is_directory(source, error)) continue;
        if (error) return false;
        std::ifstream stream(source, std::ios::binary);
        if (!stream) return false;
        while (stream) {
            if (cancellation.stop_requested()) return false;
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            for (std::streamsize index = 0; index < stream.gcount(); ++index)
                append(static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]));
        }
        if (!stream.eof()) return false;
        append(0);
    }
    fingerprint = hash;
    if (withoutMetadata) *withoutMetadata = sourceHash;
    if (rootInputFingerprint) *rootInputFingerprint = rootHash;
    if (hasInputManifest) *hasInputManifest = manifestExists;
    return true;
}

bool ComputeManagedSourceFingerprint(const std::filesystem::path& project,
    const std::filesystem::path& inputManifest, std::uint64_t& fingerprint, bool includeMetadata)
{
    return ComputeManagedSourceFingerprintImpl(project, inputManifest, fingerprint, includeMetadata);
}

bool ComputeManagedSourceSnapshot(const std::filesystem::path& project,
    const std::filesystem::path& inputManifest, ManagedSourceSnapshot& snapshot)
{
    snapshot.valid = ComputeManagedSourceFingerprintImpl(project, inputManifest, snapshot.fingerprint, true,
        &snapshot.withoutMetadataFingerprint, {}, &snapshot.rootInputFingerprint, &snapshot.hasInputManifest);
    return snapshot.valid;
}

namespace {

class ThreadedManagedSourceWatcher final : public IManagedSourceWatcher {
public:
    ~ThreadedManagedSourceWatcher() override { Stop(); }

    void Start(const std::filesystem::path& project, const std::filesystem::path& inputManifest) override
    {
        Stop();
        m_thread = std::jthread([this, project, inputManifest](std::stop_token cancellation) {
            while (!cancellation.stop_requested()) {
                ManagedSourceSnapshot snapshot;
                {
                    std::lock_guard lock(m_mutex);
                    snapshot.revision = m_requestedRevision;
                }
                snapshot.valid = ComputeManagedSourceFingerprintImpl(project, inputManifest,
                    snapshot.fingerprint, true, &snapshot.withoutMetadataFingerprint, cancellation,
                    &snapshot.rootInputFingerprint, &snapshot.hasInputManifest);
                {
                    std::lock_guard lock(m_mutex);
                    if (cancellation.stop_requested()) return;
                    m_snapshot = snapshot;
                    m_hasSnapshot = true;
                }
                std::unique_lock lock(m_mutex);
                m_wakeup.wait_for(lock, std::chrono::milliseconds(500), [this, &snapshot, cancellation] {
                    return cancellation.stop_requested() || m_requestedRevision != snapshot.revision;
                });
            }
        });
    }

    void Stop() override
    {
        if (!m_thread.joinable()) return;
        m_thread.request_stop();
        m_wakeup.notify_all();
        m_thread.join();
        std::lock_guard lock(m_mutex);
        m_hasSnapshot = false;
    }

    std::uint64_t RequestRefresh() override
    {
        std::lock_guard lock(m_mutex);
        ++m_requestedRevision;
        m_wakeup.notify_all();
        return m_requestedRevision;
    }

    bool Poll(ManagedSourceSnapshot& snapshot) override
    {
        std::unique_lock lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !m_hasSnapshot) return false;
        snapshot = m_snapshot;
        m_hasSnapshot = false;
        return true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wakeup;
    ManagedSourceSnapshot m_snapshot;
    std::uint64_t m_requestedRevision = 0;
    bool m_hasSnapshot = false;
    std::jthread m_thread;
};

} /// @note namespace

bool ManagedBuildHistory::IsTerminal(ManagedBuildState state)
{
    return state == ManagedBuildState::APPLIED || state == ManagedBuildState::FAILED ||
        state == ManagedBuildState::SUPERSEDED;
}

ManagedBuildRequest ManagedBuildHistory::WithCurrentTiming(const ManagedBuildRequest& request)
{
    ManagedBuildRequest timed = request;
    if (IsTerminal(request.state)) return timed;
    const auto now = std::chrono::steady_clock::now();
    const double stage = std::chrono::duration<double, std::milli>(now - request.stageStartedAt).count();
    timed.elapsedMilliseconds = std::chrono::duration<double, std::milli>(now - request.queuedAt).count();
    if (request.state == ManagedBuildState::QUEUED) timed.queueMilliseconds += stage;
    if (request.state == ManagedBuildState::COMPILING) timed.compileMilliseconds += stage;
    if (request.state == ManagedBuildState::APPLYING) timed.applyMilliseconds += stage;
    if (request.state == ManagedBuildState::SCANNING) timed.sourceMilliseconds += stage;
    if (request.state == ManagedBuildState::VALIDATING) timed.validationMilliseconds += stage;
    return timed;
}

const char* ManagedBuildHistory::StateName(ManagedBuildState state)
{
    switch (state) {
    case ManagedBuildState::SCANNING: return "scanning";
    case ManagedBuildState::QUEUED: return "queued";
    case ManagedBuildState::COMPILING: return "compiling";
    case ManagedBuildState::VALIDATING: return "validating";
    case ManagedBuildState::COMPILED: return "compiled";
    case ManagedBuildState::APPLYING: return "applying";
    case ManagedBuildState::APPLIED: return "applied";
    case ManagedBuildState::FAILED: return "failed";
    case ManagedBuildState::DEFERRED: return "deferred";
    case ManagedBuildState::SUPERSEDED: return "superseded";
    }
    return "failed";
}

const ManagedBuildRequest* ManagedBuildHistory::Find(std::string_view requestId) const
{
    const auto found = std::find_if(m_history.begin(), m_history.end(), [requestId](const ManagedBuildRequest& request) {
        return request.managedRequestId == requestId;
    });
    return found == m_history.end() ? nullptr : &*found;
}

ManagedBuildRequest* ManagedBuildHistory::FindMutable(std::string_view requestId)
{
    const auto found = std::find_if(m_history.begin(), m_history.end(), [requestId](const ManagedBuildRequest& request) {
        return request.managedRequestId == requestId;
    });
    return found == m_history.end() ? nullptr : &*found;
}

const ManagedBuildRequest* ManagedBuildHistory::Latest() const
{
    return m_history.empty() ? nullptr : &m_history.back();
}

std::string ManagedBuildHistory::Queue(std::uint64_t fingerprint, bool automatic, std::uint64_t previousGeneration)
{
    if (const ManagedBuildRequest* latest = Latest(); latest && !IsTerminal(latest->state)) {
        if (latest->effectiveSourceFingerprint == fingerprint) return latest->managedRequestId;
        Transition(latest->managedRequestId, ManagedBuildState::SUPERSEDED, "SOURCE_SUPERSEDED", "A newer source version was requested");
    }
    /// @note メインスレッドだけで発行し、プロジェクト切替でも要求 ID を再利用しない。
    static std::uint64_t nextRequest = 0;
    ManagedBuildRequest request;
    request.managedRequestId = "managed:" + std::to_string(++nextRequest);
    request.sourceFingerprint = fingerprint;
    request.effectiveSourceFingerprint = fingerprint;
    request.previousGeneration = previousGeneration;
    request.automatic = automatic;
    request.queuedAt = std::chrono::steady_clock::now();
    request.stageStartedAt = request.queuedAt;
    m_history.push_back(std::move(request));
    if (m_history.size() > MAX_HISTORY) m_history.pop_front();
    return m_history.back().managedRequestId;
}

void ManagedBuildHistory::BeginCompile(std::string_view requestId, std::string candidateId)
{
    if (ManagedBuildRequest* request = FindMutable(requestId); request && !IsTerminal(request->state)) {
        request->candidateId = std::move(candidateId);
        Transition(requestId, ManagedBuildState::COMPILING);
    }
}

bool ManagedBuildHistory::CompleteCompile(std::string_view requestId, std::uint64_t currentFingerprint,
    bool success, bool assemblyExists, int exitCode, const std::string& log)
{
    ManagedBuildRequest* request = FindMutable(requestId);
    if (!request) return false;
    request->exitCode = exitCode;
    request->diagnostics = ParseManagedDiagnostics(log);
    request->logFilePath = ParseManagedBuildLogPath(log);
    request->logOriginalBytes = log.size();
    constexpr std::size_t MAX_RECORDED_LOG_BYTES = 64 * 1024;
    request->log = log.size() <= MAX_RECORDED_LOG_BYTES ? log :
        "[earlier output truncated]\n" + log.substr(log.size() - MAX_RECORDED_LOG_BYTES);
    request->logRetainedBytes = request->log.size();
    if (IsTerminal(request->state)) return false;
    if (currentFingerprint != request->effectiveSourceFingerprint) {
        Transition(requestId, ManagedBuildState::SUPERSEDED, "SOURCE_SUPERSEDED", "Compiled source has been superseded");
        return false;
    }
    if (!success && exitCode == 2 && log.find("RESULT busy") != std::string::npos) {
        Transition(requestId, ManagedBuildState::QUEUED, "BUILD_BUSY", "Waiting for the shared build entry");
        return false;
    }
    if (!success && log.find("FBZZ0102") != std::string::npos) {
        Transition(requestId, ManagedBuildState::QUEUED, "SOURCE_CHANGED_DURING_BUILD", "Managed inputs changed during compilation; waiting for a fresh source snapshot");
        return false;
    }
    if (!success || !assemblyExists) {
        Transition(requestId, ManagedBuildState::FAILED, !success ? "COMPILE_FAILED" : "ASSEMBLY_MISSING",
            !success ? "Compilation failed; previous generation retained" : "The expected Scripts assembly is missing");
        return false;
    }
    Transition(requestId, ManagedBuildState::COMPILED);
    return true;
}

void ManagedBuildHistory::Transition(std::string_view requestId, ManagedBuildState state,
    std::string errorCode, std::string message)
{
    if (ManagedBuildRequest* request = FindMutable(requestId); request && !IsTerminal(request->state)) {
        const ManagedBuildRequest timed = WithCurrentTiming(*request);
        request->elapsedMilliseconds = timed.elapsedMilliseconds;
        request->queueMilliseconds = timed.queueMilliseconds;
        request->compileMilliseconds = timed.compileMilliseconds;
        request->applyMilliseconds = timed.applyMilliseconds;
        request->sourceMilliseconds = timed.sourceMilliseconds;
        request->validationMilliseconds = timed.validationMilliseconds;
        request->stageStartedAt = std::chrono::steady_clock::now();
        request->state = state;
        request->errorCode = std::move(errorCode);
        request->message = std::move(message);
    }
}

bool ManagedBuildHistory::CanRetryDiscoveredGraph(std::string_view requestId, const ManagedSourceSnapshot& captured,
    const ManagedSourceSnapshot& current) const
{
    const ManagedBuildRequest* request = Find(requestId);
    return request && !IsTerminal(request->state) && request->inputDiscoveryRetries == 0
        && captured.valid && current.valid && !captured.hasInputManifest && current.hasInputManifest
        && captured.rootInputFingerprint == current.rootInputFingerprint;
}

bool ManagedBuildHistory::RetryDiscoveredGraph(std::string_view requestId, const ManagedSourceSnapshot& captured,
    const ManagedSourceSnapshot& current, bool success, bool assemblyExists, int exitCode, const std::string& log)
{
    if (!success || !assemblyExists || exitCode || !CanRetryDiscoveredGraph(requestId, captured, current)) return false;
    SetEffectiveFingerprint(requestId, current.fingerprint);
    if (!CompleteCompile(requestId, current.fingerprint, success, assemblyExists, exitCode, log)) return false;
    ManagedBuildRequest* request = FindMutable(requestId);
    ++request->inputDiscoveryRetries;
    request->candidateId.clear();
    Transition(requestId, ManagedBuildState::QUEUED, "INPUT_GRAPH_DISCOVERED",
        "Initial dependency graph discovered; first candidate discarded, compiling once with all evaluated inputs captured");
    return true;
}

void ManagedBuildHistory::SetEffectiveFingerprint(std::string_view requestId, std::uint64_t fingerprint)
{
    if (ManagedBuildRequest* request = FindMutable(requestId); request && !IsTerminal(request->state))
        request->effectiveSourceFingerprint = fingerprint;
}

void ManagedBuildHistory::BindSourceFingerprint(std::string_view requestId, std::uint64_t fingerprint)
{
    if (ManagedBuildRequest* request = FindMutable(requestId); request && request->state == ManagedBuildState::SCANNING) {
        request->sourceFingerprint = fingerprint;
        request->effectiveSourceFingerprint = fingerprint;
        Transition(requestId, ManagedBuildState::QUEUED);
    }
}

void ManagedBuildHistory::MarkApplied(std::string_view requestId, std::uint64_t generation)
{
    if (ManagedBuildRequest* request = FindMutable(requestId); request && !IsTerminal(request->state)) {
        request->appliedGeneration = generation;
        Transition(requestId, ManagedBuildState::APPLIED);
    }
}

ManagedScriptEditor::ManagedScriptEditor(std::unique_ptr<IManagedSourceWatcher> watcher)
    : m_compiler(std::make_unique<Compiler>()),
      m_sourceWatcher(watcher ? std::move(watcher) : std::make_unique<ThreadedManagedSourceWatcher>()),
      m_publication(std::make_unique<ManagedScriptPublicationState>()) {}
ManagedScriptEditor::~ManagedScriptEditor() { CompleteGenerationCleanup(true); }

void ManagedScriptEditor::Init(const std::filesystem::path& projectRoot, const std::filesystem::path& engineRoot)
{
    FBZZ_PROFILE_SCOPE("ManagedHotReload::DiscoverProject");
    CompleteGenerationCleanup(true);
    m_sourceWatcher->Stop();
    m_projectRoot = projectRoot;
    m_engineRoot = engineRoot;
    m_discoverySeconds = 0.0f;
    m_ready = false;
    m_availabilityDiagnostic = "No <Project>.Scripts.csproj is available in the project root";
    m_applyRetrySeconds = 0.0f;
    m_collectionWaiting = false;
    std::error_code error;
    std::vector<std::filesystem::path> projects;
    std::filesystem::directory_iterator iterator(projectRoot, error);
    if (error) return;
    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        if (error) return;
        const std::filesystem::path path = iterator->path();
        if (path.extension() == ".csproj" && path.stem().wstring().ends_with(L".Scripts"))
            projects.push_back(path);
    }
    if (error) return;
    if (projects.empty()) return;
    if (projects.size() != 1) {
        m_availabilityDiagnostic = "Expected one <Project>.Scripts.csproj in the project root";
        FBZZ_LOG_ERROR("C# Scripts: expected one <Project>.Scripts.csproj in the project root");
        return;
    }
    m_project = projects.front();
    ManagedSdkBinding binding;
    if (!ResolveManagedSdkBinding(ManagedEditorExecutable(), projectRoot, engineRoot, FBZZ_CMAKE_CONFIG, binding, m_availabilityDiagnostic)) {
        FBZZ_LOG_ERROR("C# Scripts: executable SDK binding rejected: %s", m_availabilityDiagnostic.c_str());
        return;
    }
    m_engineRoot = binding.root; m_agentBuild = binding.agentBuild; m_hostDirectory = binding.hostDirectory;
    m_configuration = binding.configuration;
    if (!std::filesystem::is_regular_file(m_hostDirectory / "Fbzz.ManagedHost.dll", error) || error
        || !scene::ManagedScriptRuntime::Instance().Initialize(m_hostDirectory)) {
        m_availabilityDiagnostic = "Managed host is unavailable in the executable-bound configuration";
        FBZZ_LOG_ERROR("C# Scripts: %s", m_availabilityDiagnostic.c_str());
        return;
    }
    m_outputRoot = projectRoot / "Library" / "ManagedScripts" / m_configuration;
    wchar_t cacheScope[65]{};
    const DWORD scopeLength = GetEnvironmentVariableW(L"FBZZ_MANAGED_CACHE_SCOPE", cacheScope, 65);
    if (scopeLength != 0) {
        if (scopeLength >= 65 || !IsManagedCacheScope(std::wstring_view(cacheScope, scopeLength))) {
            m_availabilityDiagnostic = "FBZZ_MANAGED_CACHE_SCOPE must be one directory name (1..64 ASCII letters, digits, '-' or '_'; no device/generation names)";
            FBZZ_LOG_ERROR("C# Scripts: %s", m_availabilityDiagnostic.c_str());
            return;
        }
        /// @note Separate host API versions must not rewrite each other's evaluated inputs or generation receipts.
        auto scopePath = projectRoot;
        for (const auto& part : {std::filesystem::path("Library"), std::filesystem::path("ManagedScripts"),
                std::filesystem::path(m_configuration), std::filesystem::path(cacheScope)}) {
            scopePath /= part;
            if (IsManagedReparsePoint(scopePath)) {
                m_availabilityDiagnostic = "FBZZ_MANAGED_CACHE_SCOPE cannot use linked cache directories";
                FBZZ_LOG_ERROR("C# Scripts: %s", m_availabilityDiagnostic.c_str());
                return;
            }
        }
        m_outputRoot = std::move(scopePath);
    }
    std::filesystem::create_directories(m_outputRoot, error);
    if (error) {
        m_availabilityDiagnostic = "Unable to inspect sources or create the generation directory";
        FBZZ_LOG_ERROR("C# Scripts: unable to inspect sources or create the generation directory");
        return;
    }
    m_ownedGenerations.clear();
    std::ifstream owned(m_outputRoot / "owned-generations.txt", std::ios::binary);
    std::string ownedPath;
    while (std::getline(owned, ownedPath)) {
        if (!ownedPath.empty() && ownedPath.back() == '\r') ownedPath.pop_back();
        if (!ownedPath.empty()) m_ownedGenerations.push_back(std::filesystem::u8path(ownedPath));
    }
    std::string metadataDiagnostic;
    if (!LoadManagedScriptDisplayNames(m_projectRoot, metadataDiagnostic))
        FBZZ_LOG_WARN("C# Scripts: missing type names could not be restored: %s", metadataDiagnostic.c_str());
    LoadPrevious();
    m_appliedGeneration = scene::ManagedScriptRuntime::Instance().GetAppliedGeneration();
    m_ready = true;
    m_availabilityDiagnostic.clear();
    m_initialSourcePending = true;
    m_sourceSnapshot = {};
    m_sourceWatcher->Start(m_project, m_outputRoot / "inputs.txt");
}

bool ManagedScriptEditor::IsBusy() const
{
    return m_initialSourcePending || m_buildPending || !m_sourceRequestId.empty() ||
        !m_compilingRequestId.empty() || !m_pendingAssembly.empty() || m_publication->HasPendingVerification() ||
        m_metadataVerificationRevision != 0 || m_collectionWaiting ||
        (m_ready && m_compiler->GetState() != Compiler::State::Idle);
}

bool ManagedScriptEditor::RequiresExplicitEditApply() const
{
    return m_publication->RequiresExplicitEditApply();
}

void ManagedScriptEditor::QueueBuild(std::uint64_t fingerprint, bool automatic, float debounceSeconds)
{
    m_queuedRequestId = m_builds.Queue(fingerprint, automatic, m_appliedGeneration);
    m_observedFingerprint = fingerprint;
    m_pendingAssembly.clear();
    m_pendingRequestId.clear();
    m_manifestPrepared = false;
    m_collectionWaiting = false;
    m_applyRetrySeconds = 0.0f;
    m_buildPending = true;
    m_debounceSeconds = debounceSeconds;
    m_startVerificationRevision = 0;
    m_metadataVerificationRevision = 0;
}

bool ManagedScriptEditor::RequestBuild(std::string& requestId)
{
    if (!m_ready) return false;
    const bool explicitApply = m_publication->RequestExplicitApply();
    m_availabilityDiagnostic.clear();
    if (explicitApply && m_sourceSnapshot.valid && m_sourceSnapshot.fingerprint == m_buildFingerprint && m_compiler->GetState() == Compiler::State::Idle) {
        if (m_pendingAssembly.empty() && IsAssembly(m_latestPlayAssemblyDirectory, m_project)) {
            m_pendingAssembly = m_latestPlayAssemblyDirectory / (m_project.stem().wstring() + L".dll");
            m_pendingRequestId = m_builds.Queue(m_buildFingerprint, false, m_appliedGeneration);
            m_manifestPrepared = true;
        }
        if (!m_pendingAssembly.empty() && m_manifestPrepared) {
            m_metadataSourceFingerprint = m_sourceSnapshot.withoutMetadataFingerprint;
            m_metadataVerificationRevision = m_sourceWatcher->RequestRefresh();
            m_builds.Transition(m_pendingRequestId, ManagedBuildState::VALIDATING,
                "EXPLICIT_EDIT_APPLY", "Rechecking retained compiled code before applying it to Edit objects.");
            requestId = m_pendingRequestId; return true;
        }
    }
    if (!m_sourceRequestId.empty()) {
        requestId = m_sourceRequestId;
        return true;
    }
    if (const ManagedBuildRequest* latest = m_builds.Latest(); latest && !ManagedBuildHistory::IsTerminal(latest->state) &&
        m_sourceSnapshot.valid && latest->effectiveSourceFingerprint == m_sourceSnapshot.fingerprint) {
        requestId = latest->managedRequestId;
        if (m_buildPending) {
            m_startVerificationRevision = m_sourceWatcher->RequestRefresh();
            m_builds.Transition(requestId, ManagedBuildState::SCANNING);
        }
        return true;
    }
    QueueBuild(0, false, 0.0f);
    m_builds.Transition(m_queuedRequestId, ManagedBuildState::SCANNING);
    m_sourceRequestId = m_queuedRequestId;
    m_sourceRequestRevision = m_sourceWatcher->RequestRefresh();
    requestId = m_queuedRequestId;
    return true;
}

void ManagedScriptEditor::BeginPlaySession()
{
    m_publication->BeginPlay(m_sourceSnapshot);
    m_playSession = true; m_parkedAppliedGeneration = m_appliedGeneration; m_parkedAssemblyDirectory = m_appliedAssemblyDirectory;
}
void ManagedScriptEditor::EndPlaySession()
{
    if (!m_playSession) return;
    const bool newerAssembly = m_appliedAssemblyDirectory != m_parkedAssemblyDirectory;
    const bool workPending = IsBusy();
    if (newerAssembly) m_latestPlayAssemblyDirectory = m_appliedAssemblyDirectory;
    m_appliedGeneration = scene::ManagedScriptRuntime::Instance().GetAppliedGeneration();
    m_appliedAssemblyDirectory = m_parkedAssemblyDirectory; m_parkedAssemblyDirectory.clear(); m_playSession = false;
    m_publication->EndPlay(newerAssembly, workPending, m_ready ? m_sourceWatcher->RequestRefresh() : 0);
    if (m_ready) {
        m_availabilityDiagnostic = "Verifying source changes across Play/Stop before automatic Edit apply.";
    }
    if (m_publication->RequiresExplicitEditApply()) FBZZ_LOG_INFO("C# Scripts: original Edit generation restored; request Build C# Scripts to validate/apply newer code explicitly.");
}

void ManagedScriptEditor::RegisterOperator(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "script.managed_build";
    op.label = "Build C# Scripts";
    op.category = "Play";
    op.desc = "C# の現在のソース版をビルドし、Edit の安全点で適用する。";
    op.kind = OpKind::Action;
    op.exec = [](OpContext& context, const OpArgs&) {
        std::string requestId;
        if (!context.ctx.managedScripts || !context.ctx.managedScripts->RequestBuild(requestId))
            return OpResult::Err("MANAGED_BUILD_UNAVAILABLE", context.ctx.managedScripts ?
                context.ctx.managedScripts->AvailabilityDiagnostic() : "C# build service is unavailable");
        context.ctx.managedBuildBusy = true;
        OpData data = OpData::MakeObject();
        data.Set("managedRequestId", requestId);
        return OpResult::Data(std::move(data));
    };
    op.poll = [](const OpContext& context, const OpArgs&) {
        return context.ctx.managedScripts && context.ctx.managedScripts->IsAvailable()
            && !(context.ctx.packageBuild && context.ctx.packageBuild->IsRunning());
    };
    registry.Register(std::move(op));
    RegisterScriptResumeOperators(registry);
}

void ManagedScriptEditor::RejectPending(const char* code, const std::string& diagnostic)
{
    m_builds.Transition(m_pendingRequestId, ManagedBuildState::FAILED, code, diagnostic);
    m_pendingAssembly.clear();
    m_pendingRequestId.clear();
    m_collectionWaiting = false;
}

void ManagedScriptEditor::SaveOwnedGenerations()
{
    if (m_outputRoot.empty()) return;
    /// @note 新しいビルドの所有を同期保存する前に、古い非同期 ledger 保存を完了させて上書きの逆転を防ぐ。
    CompleteGenerationCleanup(true);
    FBZZ_PROFILE_SCOPE("ManagedHotReload::SaveOwnership");
    if (!util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(m_outputRoot / "owned-generations.txt"),
            OwnedGenerationLedger(m_ownedGenerations)))
        FBZZ_LOG_WARN("C# Scripts: generation ownership could not be recorded");
}

void ManagedScriptEditor::CleanupOwnedGenerations()
{
    if (m_generationCleanup.valid() || m_generationOwnershipSave.valid()) return;
    if (m_outputRoot.empty() || m_ownedGenerations.empty()) return;
    const auto compilingPending = m_compilingRequestId.empty() ? std::filesystem::path{}
        : std::filesystem::path(m_buildOutput.wstring() + L".pending");
    const auto retained = [this, &compilingPending](const std::filesystem::path& path) {
        return path == m_appliedAssemblyDirectory || path == m_previousAssemblyDirectory
            || path == m_parkedAssemblyDirectory || path == m_latestPlayAssemblyDirectory
            || path == m_pendingAssembly.parent_path()
            || (!m_compilingRequestId.empty() && (path == m_buildOutput || path == compilingPending));
    };
    /// @note 生存世代は削除候補ではない。退役済み候補と root を所有する worker だけが filesystem I/O を行う。
    /// @see Docs/design/csharp-scripting.md 編集時のコード差し替え。
    std::vector<std::filesystem::path> candidates;
    for (const auto& path : m_ownedGenerations) if (!retained(path)) candidates.push_back(path);
    if (candidates.empty()) return;
    FBZZ_PROFILE_SCOPE("ManagedHotReload::QueueGenerationCleanup");
    /// @note 候補は current/previous/parked/pending/compiling の外へ退役済みで、以後 active 世代へ戻らない。
    m_generationCleanup = TaskSystem::Submit([outputRoot = m_outputRoot, candidates = std::move(candidates)] {
        std::vector<std::filesystem::path> completed;
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(outputRoot, error);
        if (error) return completed;
        for (const auto& candidate : candidates) {
            const std::filesystem::path path = std::filesystem::weakly_canonical(candidate, error);
            const std::string name = util::FileSystem::PathToUtf8(candidate.filename());
            if (error || path.parent_path() != root || !IsOwnedManagedGenerationName(name)) {
                error.clear();
                completed.push_back(candidate);
                continue;
            }
            /// @note 所有 ledger の直下だけを削除し、Windows junction を含む reparse point をたどらない。
            if (IsManagedReparsePoint(candidate)) continue;
            std::filesystem::remove_all(candidate, error);
            if (error) {
                /// @note collectible DLL が開いていれば所有を残し、次の Edit の回収機会へ延期する。
                error.clear();
                continue;
            }
            completed.push_back(candidate);
        }
        return completed;
    });
}

void ManagedScriptEditor::CompleteGenerationCleanup(bool wait)
{
    if (m_generationCleanup.valid()) {
        if (!wait && m_generationCleanup.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        FBZZ_PROFILE_SCOPE("ManagedHotReload::CollectGenerationCleanup");
        const auto completed = m_generationCleanup.get();
        if (!completed.empty()) {
            std::erase_if(m_ownedGenerations, [&completed](const auto& path) {
                return std::find(completed.begin(), completed.end(), path) != completed.end();
            });
            /// @note 回収後の ledger 書き込みもフレームへ同期 I/O を持ち込まない。次の回収とビルド保存はこの完了を確認する。
            m_generationOwnershipSave = TaskSystem::Submit([outputRoot = m_outputRoot,
                    text = OwnedGenerationLedger(m_ownedGenerations)] {
                return util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(outputRoot / "owned-generations.txt"), text);
            });
        }
    }
    if (!m_generationOwnershipSave.valid()) return;
    if (!wait && m_generationOwnershipSave.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    if (!m_generationOwnershipSave.get()) FBZZ_LOG_WARN("C# Scripts: generation ownership could not be recorded");
}

void ManagedScriptEditor::LoadPrevious()
{
    std::ifstream stream(m_outputRoot / "current.txt", std::ios::binary);
    std::string generation;
    if (!std::getline(stream, generation) || generation.empty()) return;
    /// @note receipt の内容は世代ディレクトリ名だけとし、プロジェクト外の DLL を指定できない。
    if (!std::all_of(generation.begin(), generation.end(), [](unsigned char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') || value == '-';
    })) return;
    const std::filesystem::path directory = m_outputRoot / generation;
    if (!IsAssembly(directory, m_project)) return;
    const std::filesystem::path assembly = directory / (m_project.stem().wstring() + L".dll");
    scene::ManagedScriptRuntime& runtime = scene::ManagedScriptRuntime::Instance();
    if (!runtime.Load(assembly))
        FBZZ_LOG_ERROR("C# Scripts: previous generation could not be loaded: %s",
            runtime.LastDiagnostic().c_str());
    else if (!runtime.LastDiagnostic().empty())
        FBZZ_LOG_WARN("C# Scripts: %s", runtime.LastDiagnostic().c_str());
    if (runtime.GetAppliedGeneration() != 0) m_appliedAssemblyDirectory = directory;
}

void ManagedScriptEditor::StartBuild()
{
    FBZZ_PROFILE_SCOPE("ManagedHotReload::StartBuild");
    if (!m_sourceRequestId.empty() || m_initialSourcePending) return;
    if (m_startVerificationRevision == 0) {
        m_startVerificationRevision = m_sourceWatcher->RequestRefresh();
        m_builds.Transition(m_queuedRequestId, ManagedBuildState::SCANNING);
        return;
    }
    if (!m_sourceSnapshot.valid || m_sourceSnapshot.revision < m_startVerificationRevision) return;
    if (m_sourceSnapshot.fingerprint != m_observedFingerprint) {
        QueueBuild(m_sourceSnapshot.fingerprint, true, 0.3f);
        return;
    }
    m_startVerificationRevision = 0;
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    m_buildOutput = m_outputRoot / ("generation-" + std::to_string(stamp) + "-" + std::to_string(++m_nextBuild));
    m_ownedGenerations.push_back(m_buildOutput);
    m_ownedGenerations.push_back(std::filesystem::path(m_buildOutput.wstring() + L".pending"));
    SaveOwnedGenerations();
    Compiler::Config config;
    config.commandLine = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + m_agentBuild.wstring()
        + L"\" managed-build \"" + m_project.wstring() + L"\" -ManagedOutputDirectory \""
        + m_buildOutput.wstring() + L"\" -Preset "
        + (m_configuration == "Release" ? L"release" : m_configuration == "Debug" ? L"debug" : L"development")
        + L" -ManagedApiPath \"" + (m_hostDirectory / "Fbzz.ScriptApi.dll").wstring() + L"\""
        + L" -ManagedInputManifest \"" + (m_outputRoot / "inputs.txt").wstring() + L"\""
        + L" -ShowWarnings -MaxDiagnostics 2147483647";
    config.workingDirectory = m_agentBuild.parent_path().parent_path();
    m_buildFingerprint = m_observedFingerprint;
    m_compileSnapshot = m_sourceSnapshot;
    m_compilingRequestId = m_queuedRequestId;
    m_builds.BeginCompile(m_compilingRequestId, m_buildOutput.filename().string());
    m_buildPending = false;
    if (!m_compiler->Start(config)) {
        m_builds.Transition(m_compilingRequestId, ManagedBuildState::FAILED,
            "BUILD_START_FAILED", "Unable to start the build process");
        m_compilingRequestId.clear();
        m_compiler->Reset();
        FBZZ_LOG_ERROR("C# Scripts: unable to start the build process");
        return;
    }
    FBZZ_LOG_INFO("C# Scripts: compiling %ls", m_project.filename().wstring().c_str());
}

void ManagedScriptEditor::ApplyPending(scene::Scene* scene)
{
    if (m_pendingAssembly.empty() || !m_sourceSnapshot.valid || m_metadataVerificationRevision != 0) return;
    FBZZ_PROFILE_SCOPE("ManagedHotReload::Apply");
    if (m_sourceSnapshot.fingerprint != m_buildFingerprint) {
        QueueBuild(m_sourceSnapshot.fingerprint, true, 0.3f);
        FBZZ_LOG_INFO("C# Scripts: deferred assembly has been superseded; compiling the current source");
        return;
    }
    scene::ManagedScriptRuntime& runtime = scene::ManagedScriptRuntime::Instance();
    if (!m_manifestPrepared) {
        std::string discovery;
        std::string diagnostic;
        m_metadataSourceFingerprint = m_sourceSnapshot.withoutMetadataFingerprint;
        if (!runtime.DiscoverSourceCatalog(m_pendingAssembly, discovery) ||
            !PrepareManagedScriptManifest(m_projectRoot, m_pendingAssembly, discovery, diagnostic)) {
            FBZZ_LOG_ERROR("C# Scripts: source metadata rejected; previous generation retained: %s",
                diagnostic.empty() ? runtime.LastDiagnostic().c_str() : diagnostic.c_str());
            RejectPending("SOURCE_METADATA_REJECTED", diagnostic.empty() ? runtime.LastDiagnostic() : diagnostic);
            return;
        }
        m_metadataVerificationRevision = m_sourceWatcher->RequestRefresh();
        m_builds.Transition(m_pendingRequestId, ManagedBuildState::VALIDATING);
        return;
    }
    m_builds.Transition(m_pendingRequestId, ManagedBuildState::APPLYING);
    const bool applied = scene ? runtime.Reload(*scene, m_pendingAssembly) : runtime.Load(m_pendingAssembly);
    if (!applied) {
        if (runtime.IsCollectionPending()) {
            if (!m_collectionWaiting)
                FBZZ_LOG_INFO("C# Scripts: waiting for retired generations to be collected; previous generation retained");
            m_collectionWaiting = true;
            m_builds.Transition(m_pendingRequestId, ManagedBuildState::COMPILED,
                "COLLECTION_PENDING", "Waiting for retired generations to be collected");
            m_applyRetrySeconds = 0.1f;
            return;
        }
        m_collectionWaiting = false;
        FBZZ_LOG_ERROR("C# Scripts: candidate rejected; previous generation retained: %s", runtime.LastDiagnostic().c_str());
        RejectPending("CANDIDATE_REJECTED", runtime.LastDiagnostic());
        return;
    }
    m_collectionWaiting = false;
    m_previousAssemblyDirectory = m_appliedAssemblyDirectory;
    m_appliedAssemblyDirectory = m_pendingAssembly.parent_path();
    m_appliedGeneration = runtime.GetAppliedGeneration();
    if (!m_playSession) m_latestPlayAssemblyDirectory.clear();
    m_builds.MarkApplied(m_pendingRequestId, m_appliedGeneration);
    if (!runtime.LastDiagnostic().empty())
        FBZZ_LOG_WARN("C# Scripts: %s", runtime.LastDiagnostic().c_str());
    if (!util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(m_outputRoot / "current.txt"),
        m_pendingAssembly.parent_path().filename().string() + '\n'))
        FBZZ_LOG_WARN("C# Scripts: applied generation could not be recorded for the next launch");
    FBZZ_LOG_INFO("C# Scripts: generation applied");
    m_pendingAssembly.clear();
    m_pendingRequestId.clear();
}

void ManagedScriptEditor::Tick(scene::Scene* scene, bool inEditor, float deltaSeconds, bool otherBuildRunning)
{
    FBZZ_PROFILE_SCOPE("HotReload::ManagedScripts");
    CompleteGenerationCleanup(false);
    if (m_playSession) m_appliedGeneration = scene::ManagedScriptRuntime::Instance().GetAppliedGeneration();
    if (!m_ready) {
        m_discoverySeconds += deltaSeconds;
        if (m_discoverySeconds >= 0.5f && m_availabilityDiagnostic == "No <Project>.Scripts.csproj is available in the project root")
            Init(m_projectRoot, m_engineRoot);
        return;
    }
    {
        FBZZ_PROFILE_SCOPE("ManagedHotReload::CompilerPoll");
        m_compiler->Tick();
    }
    m_applyRetrySeconds -= deltaSeconds;
    ManagedSourceSnapshot snapshot;
    bool hasSnapshot = false;
    {
        FBZZ_PROFILE_SCOPE("ManagedHotReload::PollSource");
        hasSnapshot = m_sourceWatcher->Poll(snapshot);
    }
    if (hasSnapshot) {
        FBZZ_PROFILE_SCOPE("ManagedHotReload::ProcessSnapshot");
        m_sourceSnapshot = snapshot;
        if (!snapshot.valid) {
            m_availabilityDiagnostic = "Unable to verify managed source inputs";
            if (!m_sourceRequestId.empty() && snapshot.revision >= m_sourceRequestRevision) {
                m_builds.Transition(m_sourceRequestId, ManagedBuildState::FAILED,
                    "SOURCE_VERIFICATION_FAILED", m_availabilityDiagnostic);
                m_sourceRequestId.clear();
                m_buildPending = false;
                m_initialSourcePending = true;
            }
            if (m_startVerificationRevision != 0 && snapshot.revision >= m_startVerificationRevision) {
                m_builds.Transition(m_queuedRequestId, ManagedBuildState::FAILED,
                    "SOURCE_VERIFICATION_FAILED", m_availabilityDiagnostic);
                m_buildPending = false;
                m_startVerificationRevision = 0;
                m_initialSourcePending = true;
            }
            if (m_metadataVerificationRevision != 0 && snapshot.revision >= m_metadataVerificationRevision) {
                RejectPending("SOURCE_VERIFICATION_FAILED", m_availabilityDiagnostic);
                m_metadataVerificationRevision = 0;
                m_initialSourcePending = true;
            }
        }
        else {
            m_availabilityDiagnostic.clear();
            if (inEditor && m_publication->VerifyStoppedSource(snapshot) == ManagedStopVerification::CHANGED)
                FBZZ_LOG_INFO("C# Scripts: source changed across Play/Stop; request Build C# Scripts to validate/apply it to Edit explicitly.");
            if (!m_sourceRequestId.empty() && snapshot.revision >= m_sourceRequestRevision) {
                m_builds.BindSourceFingerprint(m_sourceRequestId, snapshot.fingerprint);
                m_observedFingerprint = snapshot.fingerprint;
                m_sourceRequestId.clear();
                m_initialSourcePending = false;
            } else if (m_metadataVerificationRevision != 0 && snapshot.revision >= m_metadataVerificationRevision) {
                m_metadataVerificationRevision = 0;
                if (snapshot.withoutMetadataFingerprint != m_metadataSourceFingerprint) {
                    QueueBuild(snapshot.fingerprint, true, 0.3f);
                } else {
                    /// @note importer 自身の metadata 変更だけを同じ要求の有効版へ取り込む。
                    m_observedFingerprint = snapshot.fingerprint;
                    m_buildFingerprint = snapshot.fingerprint;
                    m_builds.SetEffectiveFingerprint(m_pendingRequestId, snapshot.fingerprint);
                    m_manifestPrepared = true;
                    m_builds.Transition(m_pendingRequestId, ManagedBuildState::COMPILED);
                }
            } else if (!m_compilingRequestId.empty() && m_builds.CanRetryDiscoveredGraph(m_compilingRequestId, m_compileSnapshot, snapshot)) {
                const auto* request = m_builds.Find(m_compilingRequestId);
                m_builds.Transition(m_compilingRequestId, request->state, "INPUT_GRAPH_DISCOVERY",
                    "Initial dependency graph discovered; verifying completion before discarding the first candidate and retrying");
            } else if (m_sourceRequestId.empty() && m_metadataVerificationRevision == 0 &&
                (m_initialSourcePending || snapshot.fingerprint != m_observedFingerprint)) {
                QueueBuild(snapshot.fingerprint, true, m_initialSourcePending ? 0.0f : 0.3f);
                m_initialSourcePending = false;
            }
        }
    }
    if (inEditor && m_publication->HasStopVerification() && m_sourceSnapshot.valid)
        m_availabilityDiagnostic = "Waiting for a fresh post-Stop source snapshot before automatic Edit apply.";
    const Compiler::State state = m_compiler->GetState();
    if (state == Compiler::State::Done || state == Compiler::State::Failed || state == Compiler::State::Cancelled) {
        if (!m_publication->HasCompileVerification()) {
            m_publication->AwaitCompileVerification(m_sourceWatcher->RequestRefresh());
            m_builds.Transition(m_compilingRequestId, ManagedBuildState::VALIDATING);
            return;
        }
        if (!m_publication->TryCompleteCompileVerification(m_sourceSnapshot.revision)) return;
        if (!m_sourceSnapshot.valid) {
            FBZZ_LOG_ERROR("C# Scripts: cannot verify the source version after compilation");
            m_builds.Transition(m_compilingRequestId, ManagedBuildState::FAILED,
                "SOURCE_VERIFICATION_FAILED", "Cannot verify the source version after compilation");
            QueueBuild(m_observedFingerprint, true, 0.5f);
            m_compiler->Reset();
            m_compilingRequestId.clear();
            return;
        }
        const std::uint64_t currentFingerprint = m_sourceSnapshot.fingerprint;
        if (state == Compiler::State::Cancelled)
            m_builds.Transition(m_compilingRequestId, ManagedBuildState::FAILED, "BUILD_CANCELLED", "Build was cancelled");
        if (m_builds.RetryDiscoveredGraph(m_compilingRequestId, m_compileSnapshot, m_sourceSnapshot,
            state == Compiler::State::Done, IsAssembly(m_buildOutput, m_project), m_compiler->GetExitCode(), m_compiler->GetLog())) {
            m_observedFingerprint = currentFingerprint;
            m_buildFingerprint = currentFingerprint;
            m_queuedRequestId = m_compilingRequestId;
            m_buildPending = true; m_debounceSeconds = 0.0f; m_startVerificationRevision = 0;
            m_buildOutput.clear();
            m_compiler->Reset(); m_compilingRequestId.clear();
            if (inEditor) CleanupOwnedGenerations();
            return;
        }
        const bool compiled = m_builds.CompleteCompile(m_compilingRequestId, currentFingerprint,
            state == Compiler::State::Done, IsAssembly(m_buildOutput, m_project), m_compiler->GetExitCode(), m_compiler->GetLog());
        if (compiled) {
            m_pendingAssembly = m_buildOutput / (m_project.stem().wstring() + L".dll");
            m_pendingRequestId = m_compilingRequestId;
            m_manifestPrepared = false;
        } else if (state == Compiler::State::Failed) {
            /// @note AgentBuild の BUSY はコードの失敗ではなく、競合が終わってから同じ版を再試行する。
            const ManagedBuildRequest* request = m_builds.Find(m_compilingRequestId);
            if (request && request->state == ManagedBuildState::QUEUED) {
                FBZZ_LOG_INFO("C# Scripts: waiting for the shared build entry; previous generation retained");
                m_queuedRequestId = m_compilingRequestId;
                m_buildPending = true;
                m_debounceSeconds = 1.0f;
            } else FBZZ_LOG_ERROR("C# Scripts: build failed; previous generation retained\n%s", m_compiler->GetLog().c_str());
        } else if (m_buildFingerprint != m_observedFingerprint) {
            FBZZ_LOG_INFO("C# Scripts: compiled source has been superseded");
        } else if (state == Compiler::State::Done) {
            FBZZ_LOG_ERROR("C# Scripts: expected assembly is missing; AssemblyName must match the Scripts project filename");
        }
        m_compiler->Reset();
        m_compilingRequestId.clear();
    }
    if (!m_buildPending && m_applyRetrySeconds <= 0.0f && m_compiler->GetState() == Compiler::State::Idle) {
        if (!m_publication->CanAutomaticallyApply(inEditor)) {
            if (!m_pendingAssembly.empty()) m_builds.Transition(m_pendingRequestId, ManagedBuildState::DEFERRED,
                "EDIT_GENERATION_PARKED", "Original Edit objects/history are restored. Request Build C# Scripts to validate/apply newer code explicitly.");
        } else ApplyPending(scene);
    }
    if (m_buildPending) {
        m_debounceSeconds -= deltaSeconds;
        if (m_debounceSeconds <= 0.0f && !otherBuildRunning && m_compiler->GetState() == Compiler::State::Idle)
            StartBuild();
    }
    /// @note Play/Pause・Stop復元中は新しい削除を投入しない。Edit の再試行は worker へ送り、フレームでは待たない。
    /// @see Docs/design/csharp-scripting.md collectible世代の回収と所有ディレクトリ。
    m_cleanupSeconds = inEditor ? m_cleanupSeconds + deltaSeconds : 0.0f;
    if (inEditor && m_cleanupSeconds >= 1.0f) {
        m_cleanupSeconds = 0.0f;
        CleanupOwnedGenerations();
    }
}

void ManagedScriptEditor::Shutdown(scene::Scene* scene)
{
    m_sourceWatcher->Stop();
    if (m_compiler->GetState() == Compiler::State::Building) m_compiler->Cancel();
    if (m_ready) scene::ManagedScriptRuntime::Instance().Unload(scene);
    if (const ManagedBuildRequest* request = m_builds.Latest())
        m_builds.Transition(request->managedRequestId, ManagedBuildState::FAILED,
            "MANAGED_BUILD_SHUTDOWN", "Project was closed before the request completed");
    m_ready = false;
    m_buildPending = false;
    m_appliedGeneration = 0;
    m_playSession = false; m_parkedAssemblyDirectory.clear(); m_latestPlayAssemblyDirectory.clear();
    m_publication->Reset();
    m_availabilityDiagnostic = "C# build service is shut down";
    m_collectionWaiting = false;
    m_pendingAssembly.clear();
    m_pendingRequestId.clear();
    m_compilingRequestId.clear();
    m_queuedRequestId.clear();
    m_manifestPrepared = false;
    m_initialSourcePending = false;
    m_sourceRequestId.clear();
    m_sourceRequestRevision = 0;
    m_startVerificationRevision = 0;
    m_metadataVerificationRevision = 0;
    m_buildOutput.clear();
    CompleteGenerationCleanup(true);
    CleanupOwnedGenerations();
    CompleteGenerationCleanup(true);
}

} /// @note namespace fbzz::editor
