/// @file    AssetStreaming.cpp
/// @brief   要求台帳・完了キュー・フレーム境界での公開。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Core/Logger.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::asset {

namespace {

/// @brief ワーカーからメインスレッドへ返す 1 件。照合用の世代を投入時の値のまま持つ。
struct Completion {
    std::uint64_t     entryId = 0;
    std::uint64_t     attempt = 0;
    std::uint64_t     epoch = 0;
    std::uint32_t     revision = 0;
    AssetQuality      quality = 0;
    AssetDecodeResult result;
};

/// @brief ワーカーとメインスレッドの唯一の共有物。
/// @note 長さはジョブ枠 (maxJobsInFlight) で抑えるので、ワーカーが push で待つことは無い。
///       終了時に join できなくなる «満杯のキューへ詰まったワーカー» を作らないため。
struct CompletionQueue {
    std::mutex              mutex;
    std::vector<Completion> items;
};

/// 解放を断られたアセットを試し直すまでの Pump 数。
constexpr std::uint64_t kEvictionRetryPumps = 120;

enum class Phase : std::uint8_t {
    Idle, Queued, Decoding, WaitingDependencies, UploadQueued, Uploading,
};

std::string JoinKey(std::string_view typeName, std::string_view key)
{
    std::string joined;
    joined.reserve(typeName.size() + key.size() + 1);
    joined.append(typeName);
    joined.push_back('\x1f');
    joined.append(key);
    return joined;
}

std::string HandleKey(std::string_view typeName, RawAssetHandle handle)
{
    return JoinKey(typeName, std::to_string(handle.id) + ":" + std::to_string(handle.gen));
}

} // namespace

struct AssetStreamer::Impl {
    struct Entry {
        std::uint64_t id = 0;
        std::shared_ptr<IAssetStreamChannel> channel;
        std::string    typeName;
        std::string    key;
        RawAssetHandle handle;

        Phase          phase = Phase::Idle;
        bool           failed = false;
        bool           canceled = false;
        bool           updateFailed = false;
        AssetLoadError error = AssetLoadError::None;
        std::string    message;

        std::vector<std::uint64_t> leaseIds;
        AssetPriority  priority = AssetPriority::Background;
        std::uint64_t  enqueuedPump = 0;

        /// 要求実行ごとに進む。キャンセル後の再要求で旧ジョブの結果を拾わない。
        std::uint64_t  attempt = 1;
        /// 読み直しのたびに進む。これより古い結果は公開しない。
        std::uint32_t  requestedRevision = 1;
        std::uint32_t  publishedRevision = 0;

        std::unique_ptr<IDecodedAsset> decoded;
        std::uint32_t  decodedRevision = 0;
        std::vector<std::uint64_t> pendingDependencyLeases;
        std::vector<std::uint64_t> dependencyLeases;

        std::unique_ptr<IAssetCandidate> candidate;
        std::uint64_t  uploadToken = 0;
        std::uint64_t  candidateDeviceEpoch = 0;
        std::uint32_t  candidateRevision = 0;
        AssetQuality   candidateQuality = 0;

        /// @name 品質と常駐
        /// @{
        /// 次のジョブで作る品質。公開されると residentQuality になる。
        AssetQuality   requestedQuality = 0;
        AssetQuality   residentQuality = 0;
        /// 利用権から集約した希望品質と、その値になった Pump。ヒステリシスの起点。
        AssetQuality   desiredQuality = 0;
        std::uint64_t  desiredSincePump = 0;
        std::uint64_t  residentBytes = 0;
        /// 利用権がゼロになった Pump。LRU の順序と猶予の起点。
        std::uint64_t  releasedAtPump = 0;
        /// 解放を断られた Pump。猶予の間は試し直さない。
        std::uint64_t  evictionRefusedAtPump = 0;
        bool           evictionRefused = false;
        /// @}
    };

    struct Lease {
        std::uint64_t entryId = 0;
        AssetPriority priority = AssetPriority::Visible;
        AssetQuality  quality = 0;
        /// 他のアセットの依存として台帳自身が持つ利用権。
        std::uint64_t ownerEntryId = 0;
    };

    JobExecutor executor;
    Config      config;
    std::shared_ptr<CompletionQueue> completions = std::make_shared<CompletionQueue>();

    std::unordered_map<std::string, std::shared_ptr<IAssetStreamChannel>> channels;
    std::unordered_map<std::uint64_t, std::unique_ptr<Entry>> entries;
    std::unordered_map<std::string, std::uint64_t> entryByKey;
    std::unordered_map<std::string, std::uint64_t> entryByHandle;
    std::unordered_map<std::uint64_t, Lease> leases;

    std::uint64_t nextEntryId = 1;
    std::uint64_t nextLeaseId = 1;
    std::uint64_t epoch = 1;
    std::uint64_t pumpIndex = 0;
    std::uint32_t jobsInFlight = 0;
    std::uint64_t staleCompletionsDropped = 0;
    std::uint64_t syncMisses = 0;
    std::uint64_t uploadedBytesLastPump = 0;
    std::uint32_t uploadsThisPump = 0;
    std::uint64_t syncLoads = 0;
    std::uint64_t evictions = 0;
    std::uint32_t deferredPrefetches = 0;
    /// 公開済み実体 + 保持中の CPU 成果物。Pump のたびに数え直す。
    std::uint64_t residentBytes = 0;
    bool          overBudget = false;
    bool          overBudgetReported = false;
    /// 予算超過が続く間、全アセットの希望品質へ上乗せする段数。
    AssetQuality  qualityBias = 0;
    std::uint64_t qualityBiasChangedPump = 0;
    std::unordered_set<std::string> reportedSyncMisses;

    Entry* FindEntry(std::uint64_t id)
    {
        const auto it = entries.find(id);
        return it != entries.end() ? it->second.get() : nullptr;
    }
    const Entry* FindEntry(std::uint64_t id) const
    {
        const auto it = entries.find(id);
        return it != entries.end() ? it->second.get() : nullptr;
    }
    Entry* FindByHandle(std::string_view typeName, RawAssetHandle handle)
    {
        const auto it = entryByHandle.find(HandleKey(typeName, handle));
        return it != entryByHandle.end() ? FindEntry(it->second) : nullptr;
    }
    const Entry* FindByHandle(std::string_view typeName, RawAssetHandle handle) const
    {
        const auto it = entryByHandle.find(HandleKey(typeName, handle));
        return it != entryByHandle.end() ? FindEntry(it->second) : nullptr;
    }
    Entry* FindByKey(std::string_view typeName, const std::string& key)
    {
        const auto it = entryByKey.find(JoinKey(typeName, key));
        return it != entryByKey.end() ? FindEntry(it->second) : nullptr;
    }

    [[nodiscard]] static bool IsPublished(const Entry& entry) { return entry.publishedRevision != 0; }

    [[nodiscard]] static AssetLoadState StateOf(const Entry& entry)
    {
        if (IsPublished(entry)) return AssetLoadState::Ready;
        if (entry.failed)       return AssetLoadState::Failed;
        if (entry.canceled)     return AssetLoadState::Canceled;
        switch (entry.phase) {
        case Phase::Queued:              return AssetLoadState::Queued;
        case Phase::Decoding:            return AssetLoadState::Decoding;
        case Phase::WaitingDependencies: return AssetLoadState::WaitingDependencies;
        case Phase::UploadQueued:        return AssetLoadState::UploadQueued;
        case Phase::Uploading:           return AssetLoadState::Uploading;
        case Phase::Idle:                break;
        }
        return AssetLoadState::Queued;
    }

    /// @brief 利用権から優先度と希望品質を集約する。どちらも «最も要求の強い 1 件» に合わせる。
    void RecomputePriority(Entry& entry)
    {
        AssetPriority best = AssetPriority::Background;
        AssetQuality  quality = 0xFF;
        for (const std::uint64_t leaseId : entry.leaseIds) {
            const auto it = leases.find(leaseId);
            if (it == leases.end()) continue;
            if (it->second.priority < best) best = it->second.priority;
            quality = (std::min)(quality, it->second.quality);
        }
        entry.priority = best;
        /// @note 利用権が無いときは希望を動かさない。直前の品質のまま猶予を待って解放される。
        if (entry.leaseIds.empty()) return;
        if (quality != entry.desiredQuality) {
            entry.desiredQuality = quality;
            entry.desiredSincePump = pumpIndex;
        }
    }

    /// @brief 予算による上乗せを足し、経路の最低品質で止めた実効の希望品質。
    [[nodiscard]] AssetQuality EffectiveQuality(const Entry& entry) const
    {
        if (!entry.channel->SupportsQuality()) return 0;
        const unsigned biased = static_cast<unsigned>(entry.desiredQuality) + qualityBias;
        return static_cast<AssetQuality>((std::min)(biased, static_cast<unsigned>(entry.channel->LowestQuality())));
    }

    /// @brief 公開済み実体の常駐バイト数。非同期で作ったものは台帳が数え、同期で入ったものは経路に見積もらせる。
    [[nodiscard]] std::uint64_t MeasureResident(const Entry& entry) const
    {
        if (entry.residentBytes != 0) return entry.residentBytes;
        return entry.channel->EstimateResidentBytes(entry.handle);
    }

    [[nodiscard]] AssetLoadStatus MakeStatus(const Entry* entry) const
    {
        AssetLoadStatus status;
        if (!entry) return status;
        status.state = StateOf(*entry);
        status.error = entry->error;
        status.publishedRevision = entry->publishedRevision;
        status.updateFailed = entry->updateFailed;
        status.residentQuality = entry->residentQuality;
        status.desiredQuality = EffectiveQuality(*entry);
        status.residentBytes = IsPublished(*entry) ? MeasureResident(*entry) : 0;
        status.message = entry->message;
        return status;
    }

    void Enqueue(Entry& entry)
    {
        entry.phase = Phase::Queued;
        entry.enqueuedPump = pumpIndex;
    }

    void DiscardCandidate(Entry& entry)
    {
        if (!entry.candidate) return;
        const bool deviceValid = entry.channel->DeviceEpoch() == entry.candidateDeviceEpoch;
        entry.channel->Discard(*entry.candidate, deviceValid);
        entry.candidate.reset();
        entry.uploadToken = 0;
    }

    void ReleaseLeaseList(std::vector<std::uint64_t>& list)
    {
        /// @note 依存の解放が連鎖してこの Entry 自身を消すことは無い (親が依存を持つ向きしか無い) が、
        ///       Release 中に list が書き換わらないよう先に取り出す。
        std::vector<std::uint64_t> taken;
        taken.swap(list);
        for (const std::uint64_t leaseId : taken) ReleaseLease(leaseId);
    }

    /// @brief 公開前の作業をすべて捨てる。実行中ジョブの結果は attempt の不一致で棄却される。
    void AbandonWork(Entry& entry)
    {
        ++entry.attempt;
        entry.phase = Phase::Idle;
        entry.decoded.reset();
        entry.decodedRevision = 0;
        DiscardCandidate(entry);
        ReleaseLeaseList(entry.pendingDependencyLeases);
    }

    void Fail(Entry& entry, AssetLoadError error, std::string message)
    {
        if (IsPublished(entry)) {
            /// @note 公開中の実体は残す。更新失敗だけを記録する。
            entry.updateFailed = true;
            entry.message = std::move(message);
            entry.phase = Phase::Idle;
            entry.decoded.reset();
            DiscardCandidate(entry);
            ReleaseLeaseList(entry.pendingDependencyLeases);
            return;
        }
        AbandonWork(entry);
        entry.failed = true;
        entry.error = error;
        entry.message = std::move(message);
    }

    void ReleaseLease(std::uint64_t leaseId)
    {
        const auto leaseIt = leases.find(leaseId);
        if (leaseIt == leases.end()) return;
        const std::uint64_t entryId = leaseIt->second.entryId;
        leases.erase(leaseIt);

        Entry* entry = FindEntry(entryId);
        if (!entry) return;
        entry->leaseIds.erase(std::remove(entry->leaseIds.begin(), entry->leaseIds.end(), leaseId),
                              entry->leaseIds.end());
        RecomputePriority(*entry);
        if (!entry->leaseIds.empty()) return;
        /// @note 公開済みは猶予の後に LRU で解放する (EvictUnused)。ここでは起点だけ記録する。
        if (IsPublished(*entry)) {
            entry->releasedAtPump = pumpIndex;
            entry->evictionRefused = false;
            return;
        }
        /// @note 失敗した要求はスロットごと残す。返すと解放→再要求のたびに投入し直す失敗ループになる。
        ///       再試行は Retry だけが行う。
        if (entry->failed) return;

        /// @note 利用者が居なくなった未公開の要求は取り下げる。予約したスロットも返し、
        ///       世代を進めて古いハンドルを無効にする。
        AbandonWork(*entry);
        entry->canceled = true;
        if (entry->handle.IsValid()) {
            entryByHandle.erase(HandleKey(entry->typeName, entry->handle));
            if (entry->channel->IsLive(entry->handle)) entry->channel->Free(entry->handle, entry->key);
            entry->handle = {};
        }
    }

    std::uint64_t AddLease(Entry& entry, AssetPriority priority, AssetQuality quality, std::uint64_t ownerEntryId)
    {
        const std::uint64_t leaseId = nextLeaseId++;
        leases[leaseId] = Lease{ entry.id, priority, quality, ownerEntryId };
        entry.leaseIds.push_back(leaseId);
        RecomputePriority(entry);
        return leaseId;
    }

    RawRequest Request(std::string_view typeName, const std::string& reference,
                       AssetPriority priority, AssetQuality quality, std::uint64_t ownerEntryId)
    {
        const auto channelIt = channels.find(std::string(typeName));
        if (channelIt == channels.end()) return { 0, {}, AssetLoadError::NoChannel };
        const std::shared_ptr<IAssetStreamChannel>& channel = channelIt->second;
        const std::string key = channel->MakeKey(reference);

        if (Entry* existing = FindByKey(typeName, key)) {
            if (!existing->handle.IsValid() || !existing->channel->IsLive(existing->handle)) {
                /// @note 取り下げでスロットを返した後の再要求。新しいスロットと新しい attempt で始め直す。
                existing->handle = existing->channel->Reserve(key);
                entryByHandle[HandleKey(typeName, existing->handle)] = existing->id;
                existing->canceled = false;
                existing->failed = false;
                existing->error = AssetLoadError::None;
                existing->message.clear();
                ++existing->attempt;
                Enqueue(*existing);
            } else if (existing->canceled) {
                existing->canceled = false;
                ++existing->attempt;
                Enqueue(*existing);
            }
            const std::uint64_t leaseId = AddLease(*existing, priority, quality, ownerEntryId);
            if (existing->phase == Phase::Queued) existing->requestedQuality = EffectiveQuality(*existing);
            return { leaseId, existing->handle, AssetLoadError::None };
        }

        if (entries.size() >= config.maxEntries) {
            FBZZ_LOG_WARN("AssetStreamer: 台帳が満杯です (maxEntries=%u)。要求を拒否しました [%s]",
                          config.maxEntries, reference.c_str());
            return { 0, {}, AssetLoadError::BudgetExceeded };
        }
        /// @note 解放できるものを解放しても予算を超えている = 固定物だけで溢れている。無制限に確保して
        ///       解決せず、新しいキーを断って呼び出し側に知らせる。
        if (overBudget) return { 0, {}, AssetLoadError::BudgetExceeded };

        auto entry = std::make_unique<Entry>();
        entry->id = nextEntryId++;
        entry->channel = channel;
        entry->typeName = std::string(typeName);
        entry->key = key;

        const RawAssetHandle cached = channel->Lookup(key);
        if (cached.IsValid() && channel->IsFilled(cached)) {
            /// @note 同期 Load<T> で既に読まれている。ジョブを出さずそのまま READY にする。
            entry->handle = cached;
            entry->publishedRevision = entry->requestedRevision;
        } else {
            entry->handle = channel->Reserve(key);
            Enqueue(*entry);
        }

        Entry& stored = *entry;
        entryByKey[JoinKey(typeName, key)] = stored.id;
        entryByHandle[HandleKey(typeName, stored.handle)] = stored.id;
        entries.emplace(stored.id, std::move(entry));
        const std::uint64_t leaseId = AddLease(stored, priority, quality, ownerEntryId);
        /// @note 初回のロードは最初から希望品質で作る。同期で入っていた実体は最高品質として扱う。
        stored.desiredSincePump = pumpIndex;
        if (IsPublished(stored)) stored.residentQuality = 0;
        else stored.requestedQuality = EffectiveQuality(stored);
        return { leaseId, stored.handle, AssetLoadError::None };
    }

    /// @brief from から必須依存を辿って target に届くか。届いたら path に経路を残す。
    bool ReachesThroughRequiredDependencies(std::uint64_t fromId, std::uint64_t targetId,
                                            std::unordered_set<std::uint64_t>& visited,
                                            std::vector<std::string>& path) const
    {
        if (fromId == targetId) return true;
        if (!visited.insert(fromId).second) return false;
        const Entry* from = FindEntry(fromId);
        if (!from || !from->decoded) return false;
        for (const std::uint64_t leaseId : from->pendingDependencyLeases) {
            const auto it = leases.find(leaseId);
            if (it == leases.end()) continue;
            if (!IsRequiredDependency(*from, it->second.entryId)) continue;
            path.push_back(from->key);
            if (ReachesThroughRequiredDependencies(it->second.entryId, targetId, visited, path)) return true;
            path.pop_back();
        }
        return false;
    }

    [[nodiscard]] bool IsRequiredDependency(const Entry& parent, std::uint64_t dependencyEntryId) const
    {
        const Entry* dependency = FindEntry(dependencyEntryId);
        if (!dependency || !parent.decoded) return false;
        for (const AssetDependency& declared : parent.decoded->dependencies) {
            if (!declared.required || declared.typeName != dependency->typeName) continue;
            if (dependency->channel->MakeKey(declared.reference) == dependency->key) return true;
        }
        return false;
    }

    void AcceptDecoded(Entry& entry, std::unique_ptr<IDecodedAsset> decoded, std::uint32_t revision,
                       AssetQuality quality)
    {
        ReleaseLeaseList(entry.pendingDependencyLeases);
        entry.decoded = std::move(decoded);
        entry.decodedRevision = revision;
        entry.candidateQuality = quality;

        for (const AssetDependency& dependency : entry.decoded->dependencies) {
            const RawRequest request = Request(dependency.typeName, dependency.reference, entry.priority, 0, entry.id);
            if (request.leaseId == 0) {
                if (!dependency.required) continue;
                Fail(entry, AssetLoadError::DependencyFailed,
                     "必須依存を要求できません: " + dependency.reference);
                return;
            }
            entry.pendingDependencyLeases.push_back(request.leaseId);
        }

        /// @note 循環はワーカー同士の待ち合わせにせず、ここで経路付きの失敗にする。
        for (const std::uint64_t leaseId : entry.pendingDependencyLeases) {
            const auto it = leases.find(leaseId);
            if (it == leases.end() || !IsRequiredDependency(entry, it->second.entryId)) continue;
            std::unordered_set<std::uint64_t> visited;
            std::vector<std::string> path;
            if (ReachesThroughRequiredDependencies(it->second.entryId, entry.id, visited, path)) {
                std::string message = "必須依存が循環しています: " + entry.key;
                for (const std::string& step : path) message += " -> " + step;
                message += " -> " + entry.key;
                Fail(entry, AssetLoadError::DependencyCycle, std::move(message));
                return;
            }
        }
        entry.phase = Phase::WaitingDependencies;
    }

    /// @return 状態が変わったら true。
    bool ResolveDependencies(Entry& entry)
    {
        if (entry.phase != Phase::WaitingDependencies) return false;
        for (const std::uint64_t leaseId : entry.pendingDependencyLeases) {
            const auto it = leases.find(leaseId);
            if (it == leases.end()) continue;
            if (!IsRequiredDependency(entry, it->second.entryId)) continue;
            const Entry* dependency = FindEntry(it->second.entryId);
            if (!dependency) continue;
            const AssetLoadState state = StateOf(*dependency);
            if (state == AssetLoadState::Ready) continue;
            if (state == AssetLoadState::Failed || state == AssetLoadState::Canceled) {
                std::string message = "必須依存の読み込みに失敗しました: " + dependency->key;
                if (!dependency->message.empty()) message += " (" + dependency->message + ")";
                Fail(entry, AssetLoadError::DependencyFailed, std::move(message));
                return true;
            }
            return false;
        }
        entry.phase = Phase::UploadQueued;
        entry.enqueuedPump = pumpIndex;
        return true;
    }

    void DrainCompletions()
    {
        std::vector<Completion> drained;
        {
            std::lock_guard lock(completions->mutex);
            drained.swap(completions->items);
        }
        for (Completion& completion : drained) {
            if (jobsInFlight > 0) --jobsInFlight;
            Entry* entry = FindEntry(completion.entryId);
            /// @note 受け取った時点で照合する。公開直前にも revision / deviceEpoch をもう一度見る。
            if (!entry || completion.epoch != epoch || completion.attempt != entry->attempt
                || completion.revision != entry->requestedRevision) {
                ++staleCompletionsDropped;
                continue;
            }
            if (!completion.result.decoded) {
                const AssetLoadError error = completion.result.error == AssetLoadError::None
                    ? AssetLoadError::DecodeFailed : completion.result.error;
                FBZZ_LOG_WARN("AssetStreamer: 読み込み失敗 [%s] %s", entry->key.c_str(),
                              completion.result.message.c_str());
                Fail(*entry, error, std::move(completion.result.message));
                continue;
            }
            AcceptDecoded(*entry, std::move(completion.result.decoded), completion.revision, completion.quality);
        }
    }

    void AdvanceDependencies()
    {
        /// @note 依存の失敗は親へ、親の失敗はさらにその親へ伝わる。変化が止まるまで回す。
        bool changed = true;
        for (std::size_t guard = 0; changed && guard <= entries.size(); ++guard) {
            changed = false;
            for (auto& [id, entry] : entries)
                changed |= ResolveDependencies(*entry);
        }
    }

    std::vector<Entry*> Collect(Phase phase)
    {
        std::vector<Entry*> result;
        for (auto& [id, entry] : entries)
            if (entry->phase == phase) result.push_back(entry.get());
        std::sort(result.begin(), result.end(), [](const Entry* a, const Entry* b) {
            if (a->priority != b->priority) return a->priority < b->priority;
            if (a->enqueuedPump != b->enqueuedPump) return a->enqueuedPump < b->enqueuedPump;
            return a->id < b->id;
        });
        return result;
    }

    void SubmitUploads()
    {
        for (Entry* entry : Collect(Phase::UploadQueued)) {
            if (uploadsThisPump >= config.maxUploadsPerPump) break;
            const std::uint64_t bytes = entry->decoded ? entry->decoded->CpuBytes() : 0;
            /// @note 1 件だけで上限を超える転送も、その Pump の最初の 1 件なら通す。
            ///       通さないと永遠に投入されない。
            if (uploadsThisPump > 0 && uploadedBytesLastPump + bytes > config.maxUploadBytesPerPump) break;

            std::uint64_t token = 0;
            std::string error;
            std::unique_ptr<IAssetCandidate> candidate = entry->decoded
                ? entry->channel->BeginUpload(*entry->decoded, token, error) : nullptr;
            if (!candidate) {
                Fail(*entry, AssetLoadError::UploadFailed, error.empty() ? "転送を投入できません" : error);
                continue;
            }
            entry->candidate = std::move(candidate);
            entry->uploadToken = token;
            entry->candidateDeviceEpoch = entry->channel->DeviceEpoch();
            entry->candidateRevision = entry->decodedRevision;
            entry->phase = Phase::Uploading;
            uploadedBytesLastPump += bytes;
            ++uploadsThisPump;
        }
    }

    void PublishCompletedUploads()
    {
        for (Entry* entry : Collect(Phase::Uploading)) {
            if (entry->channel->DeviceEpoch() != entry->candidateDeviceEpoch) {
                /// @note デバイスが作り直された。GPU 候補は捨て、残してある CPU 成果物から転送し直す。
                DiscardCandidate(*entry);
                entry->phase = Phase::UploadQueued;
                continue;
            }
            if (!entry->channel->IsUploadComplete(entry->uploadToken)) continue;

            if (!entry->channel->IsLive(entry->handle)) {
                ++staleCompletionsDropped;
                DiscardCandidate(*entry);
                entry->phase = Phase::Idle;
                continue;
            }
            if (entry->candidateRevision != entry->requestedRevision) {
                ++staleCompletionsDropped;
                DiscardCandidate(*entry);
                entry->decoded.reset();
                Enqueue(*entry);
                continue;
            }
            if (!entry->channel->Publish(entry->handle, *entry->candidate)) {
                Fail(*entry, AssetLoadError::UploadFailed, "公開先のスロットが無効です");
                continue;
            }
            entry->candidate.reset();
            entry->uploadToken = 0;
            entry->publishedRevision = entry->candidateRevision;
            entry->residentQuality = entry->candidateQuality;
            entry->residentBytes = entry->decoded ? entry->decoded->CpuBytes() : 0;
            entry->updateFailed = false;
            entry->failed = false;
            entry->error = AssetLoadError::None;
            entry->message.clear();
            entry->phase = Phase::Idle;
            /// @note CPU 成果物は公開後に手放す。依存の利用権は公開中の中身に付け替える。
            entry->decoded.reset();
            ReleaseLeaseList(entry->dependencyLeases);
            entry->dependencyLeases.swap(entry->pendingDependencyLeases);
        }
    }

    void SubmitJobs()
    {
        deferredPrefetches = 0;
        for (Entry* entry : Collect(Phase::Queued)) {
            if (jobsInFlight >= config.maxJobsInFlight) break;
            /// @note 予算不足の間は先読み・背景の読み込みを延期する。必須と可視は止めない。
            if (overBudget && entry->priority >= AssetPriority::Prefetch) {
                ++deferredPrefetches;
                continue;
            }
            AssetJobInput input;
            /// @note 品質段は Snapshot の前に入れる。経路は «その品質で GPU 実体が既にあるか» の判定に使う。
            input.key = entry->key;
            input.quality = entry->requestedQuality;
            std::string error;
            if (!entry->channel->Snapshot(entry->key, input, error)) {
                Fail(*entry, AssetLoadError::NotFound, error.empty() ? "参照を解決できません" : error);
                continue;
            }

            entry->phase = Phase::Decoding;
            ++jobsInFlight;
            executor([channel = entry->channel, queue = completions, entryId = entry->id,
                      attempt = entry->attempt, jobEpoch = epoch, revision = entry->requestedRevision,
                      input = std::move(input)]() {
                Completion completion;
                completion.entryId = entryId;
                completion.attempt = attempt;
                completion.epoch = jobEpoch;
                completion.revision = revision;
                completion.quality = input.quality;
                completion.result = channel->Decode(input);
                std::lock_guard lock(queue->mutex);
                queue->items.push_back(std::move(completion));
            });
        }
    }

    /// @brief 常駐量を数え直し、利用権の無い公開済みアセットを LRU で解放する。
    /// @note 猶予 (evictionGracePumps) を過ぎたものは予算に関係なく解放し、予算超過中は猶予を待たない。
    ///       «解放要求を出しただけ» では空きに数えない: 経路が Evict を受け入れたものだけを引く
    ///       (GPU 実体そのものの返却はバックエンドのフェンス管理が遅らせる)。
    void MeasureAndEvict()
    {
        std::uint64_t total = 0;
        std::vector<Entry*> candidates;
        for (auto& [id, entry] : entries) {
            if (IsPublished(*entry)) total += MeasureResident(*entry);
            if (entry->decoded) total += entry->decoded->CpuBytes();
            if (!IsPublished(*entry) || !entry->leaseIds.empty() || entry->phase != Phase::Idle) continue;
            /// @note 断られたものは一定の間を空けて試し直す (同期 Load の固定が外れていることがある)。
            if (entry->evictionRefused && pumpIndex - entry->evictionRefusedAtPump < kEvictionRetryPumps)
                continue;
            candidates.push_back(entry.get());
        }
        std::sort(candidates.begin(), candidates.end(), [](const Entry* a, const Entry* b) {
            if (a->releasedAtPump != b->releasedAtPump) return a->releasedAtPump < b->releasedAtPump;
            return a->id < b->id;
        });

        const std::uint64_t budget = config.residentBudgetBytes;
        for (Entry* entry : candidates) {
            const bool graceExpired = config.evictionGracePumps != 0
                && pumpIndex - entry->releasedAtPump >= config.evictionGracePumps;
            const bool needRoom = budget != 0 && total > budget;
            if (!graceExpired && !needRoom) break;
            const std::uint64_t bytes = MeasureResident(*entry);
            if (!entry->channel->Evict(entry->handle, entry->key)) {
                entry->evictionRefused = true;
                entry->evictionRefusedAtPump = pumpIndex;
                continue;
            }
            total -= (std::min)(total, bytes);
            ++evictions;
            /// @note スロットは経路が返し済み。DropEntry に二重に Free させない。
            entryByHandle.erase(HandleKey(entry->typeName, entry->handle));
            entry->handle = {};
            DropEntry(*entry);
        }

        residentBytes = total;
        const bool wasOverBudget = overBudget;
        overBudget = budget != 0 && total > budget;
        if (overBudget && !overBudgetReported) {
            FBZZ_LOG_WARN("AssetStreamer: 利用中のアセットだけで常駐予算を超えています (%llu / %llu bytes)。"
                          "先読みを止め、品質を落とし、新しい要求を断ります",
                          static_cast<unsigned long long>(total), static_cast<unsigned long long>(budget));
            overBudgetReported = true;
        }
        if (!overBudget) overBudgetReported = false;

        /// @note 任意の高品質表現を落として空きを作る。前の段の作り直しが着地してから次を積む
        ///       (着地前に積むと一気に最低品質まで落ちる)。戻すのは予算の 1/4 を下回ってから:
        ///       テクスチャは 1 段戻すと面積が 4 倍になり、それ未満で戻すと超過と回復を往復する。
        bool qualityWorkInFlight = false;
        for (const auto& [id, entry] : entries)
            qualityWorkInFlight |= IsPublished(*entry) && entry->phase != Phase::Idle;
        const bool held = pumpIndex - qualityBiasChangedPump >= config.qualityHoldPumps;
        if (overBudget && wasOverBudget && held && !qualityWorkInFlight && qualityBias < 8) {
            ++qualityBias;
            qualityBiasChangedPump = pumpIndex;
        } else if (!overBudget && qualityBias > 0 && held && !qualityWorkInFlight && total < budget / 4) {
            --qualityBias;
            qualityBiasChangedPump = pumpIndex;
        }
    }

    /// @brief 希望品質が保持期間を越えて続いたら、公開中の実体を残したままその品質で作り直す。
    void UpdateQuality()
    {
        for (auto& [id, entry] : entries) {
            if (!IsPublished(*entry) || entry->phase != Phase::Idle || entry->leaseIds.empty()) continue;
            if (!entry->channel->SupportsQuality()) continue;
            const AssetQuality target = EffectiveQuality(*entry);
            if (target == entry->residentQuality) continue;
            /// @note 同じ品質で失敗したら、希望が変わるまで試し直さない。
            if (entry->updateFailed && entry->requestedQuality == target) continue;
            /// @note 予算による上乗せはそれ自体が間隔を空けて動くので、ここでは利用者の希望だけを待つ。
            if (pumpIndex - entry->desiredSincePump < config.qualityHoldPumps) continue;
            entry->requestedQuality = target;
            ++entry->requestedRevision;
            Enqueue(*entry);
        }
    }

    void DropEntry(Entry& entry)
    {
        AbandonWork(entry);
        ReleaseLeaseList(entry.dependencyLeases);
        for (const std::uint64_t leaseId : entry.leaseIds) leases.erase(leaseId);
        entryByKey.erase(JoinKey(entry.typeName, entry.key));
        if (entry.handle.IsValid()) entryByHandle.erase(HandleKey(entry.typeName, entry.handle));
        entries.erase(entry.id);
    }
};

AssetStreamer& AssetStreamer::Engine()
{
    static AssetStreamer streamer([](std::function<void()> job) {
        /// @note TaskSystem が止まった後 (終了処理中) はその場で実行する。結果は epoch で棄却される。
        if (TaskSystem::WorkerCount() > 0) (void)TaskSystem::Submit(std::move(job));
        else job();
    });
    return streamer;
}

AssetStreamer::AssetStreamer(JobExecutor executor)
    : AssetStreamer(std::move(executor), Config{})
{
}

AssetStreamer::AssetStreamer(JobExecutor executor, Config config)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->executor = std::move(executor);
    m_impl->config = config;
    if (m_impl->config.maxJobsInFlight == 0) m_impl->config.maxJobsInFlight = 1;
    if (m_impl->config.maxUploadsPerPump == 0) m_impl->config.maxUploadsPerPump = 1;
}

/// @note 候補の Discard は呼ばない。静的破棄の時点で ResourceManager は既に無い。
///       GPU 実体を返すのは ResetForProjectSwitch (AssetManager::UnloadAll) の役目。
AssetStreamer::~AssetStreamer() = default;

void AssetStreamer::RegisterChannel(std::shared_ptr<IAssetStreamChannel> channel)
{
    if (!channel) return;
    m_impl->channels[std::string(channel->TypeName())] = std::move(channel);
}

AssetStreamer::RawRequest AssetStreamer::RequestRaw(std::string_view typeName, const std::string& reference,
                                                    const AssetRequestOptions& options)
{
    return m_impl->Request(typeName, reference, options.priority, options.quality, 0);
}

AssetLoadStatus AssetStreamer::GetStatusRaw(std::string_view typeName, RawAssetHandle handle) const
{
    return m_impl->MakeStatus(m_impl->FindByHandle(typeName, handle));
}

AssetLoadStatus AssetStreamer::GetLeaseStatus(std::uint64_t leaseId) const
{
    const auto it = m_impl->leases.find(leaseId);
    return m_impl->MakeStatus(it != m_impl->leases.end() ? m_impl->FindEntry(it->second.entryId) : nullptr);
}

void AssetStreamer::SetLeaseQuality(std::uint64_t leaseId, AssetQuality quality)
{
    const auto it = m_impl->leases.find(leaseId);
    if (it == m_impl->leases.end()) return;
    it->second.quality = quality;
    Impl::Entry* entry = m_impl->FindEntry(it->second.entryId);
    if (!entry) return;
    m_impl->RecomputePriority(*entry);
    /// @note まだ投入していない初回ロードは、待たずにその品質で作る。
    if (!Impl::IsPublished(*entry) && entry->phase == Phase::Queued)
        entry->requestedQuality = m_impl->EffectiveQuality(*entry);
}

bool AssetStreamer::CompleteNowRaw(std::string_view typeName, RawAssetHandle handle)
{
    Impl::Entry* entry = m_impl->FindByHandle(typeName, handle);
    if (!entry) return false;
    if (Impl::IsPublished(*entry)) return true;
    if (!entry->channel->LoadImmediately(entry->handle, entry->key)) return false;
    ++m_impl->syncLoads;
    /// @note 実行中の非同期結果は attempt の不一致で棄却される。同期ロードは最高品質で作る。
    m_impl->AbandonWork(*entry);
    entry->failed = false;
    entry->canceled = false;
    entry->error = AssetLoadError::None;
    entry->message.clear();
    entry->publishedRevision = entry->requestedRevision;
    entry->residentQuality = 0;
    entry->residentBytes = 0;
    return true;
}

void* AssetStreamer::TryGetRaw(std::string_view typeName, RawAssetHandle handle) const
{
    const Impl::Entry* entry = m_impl->FindByHandle(typeName, handle);
    if (!entry || !Impl::IsPublished(*entry)) return nullptr;
    return entry->channel->Get(handle);
}

bool AssetStreamer::RetryRaw(std::string_view typeName, RawAssetHandle handle)
{
    Impl::Entry* entry = m_impl->FindByHandle(typeName, handle);
    if (!entry) return false;
    if (Impl::IsPublished(*entry)) return entry->updateFailed ? ReloadRaw(typeName, handle) : false;
    if (!entry->failed) return false;
    m_impl->AbandonWork(*entry);
    entry->failed = false;
    entry->error = AssetLoadError::None;
    entry->message.clear();
    m_impl->Enqueue(*entry);
    return true;
}

bool AssetStreamer::ReloadRaw(std::string_view typeName, RawAssetHandle handle)
{
    Impl::Entry* entry = m_impl->FindByHandle(typeName, handle);
    if (!entry || entry->failed || entry->canceled) return false;
    /// @note 実行中の古い候補は revision の不一致で捨てられる。完了順が逆転しても新しい方だけが残る。
    ++entry->requestedRevision;
    entry->decoded.reset();
    m_impl->DiscardCandidate(*entry);
    m_impl->ReleaseLeaseList(entry->pendingDependencyLeases);
    m_impl->Enqueue(*entry);
    return true;
}

void AssetStreamer::ReleaseLease(std::uint64_t leaseId)
{
    m_impl->ReleaseLease(leaseId);
}

void AssetStreamer::SetLeasePriority(std::uint64_t leaseId, AssetPriority priority)
{
    const auto it = m_impl->leases.find(leaseId);
    if (it == m_impl->leases.end()) return;
    it->second.priority = priority;
    if (Impl::Entry* entry = m_impl->FindEntry(it->second.entryId)) m_impl->RecomputePriority(*entry);
}

void AssetStreamer::Pump()
{
    Impl& impl = *m_impl;
    impl.uploadedBytesLastPump = 0;
    impl.uploadsThisPump = 0;
    impl.DrainCompletions();
    impl.AdvanceDependencies();
    impl.SubmitUploads();
    impl.PublishCompletedUploads();
    /// @note 公開で依存が揃った親を同じ Pump のうちに進める。転送の件数・バイト予算は 1 回目と共有する。
    impl.AdvanceDependencies();
    impl.SubmitUploads();
    impl.PublishCompletedUploads();
    /// @note 公開を終えてから数える。数えた結果で先読みの延期と品質の上乗せが決まる。
    impl.MeasureAndEvict();
    impl.UpdateQuality();
    impl.SubmitJobs();
    ++impl.pumpIndex;
}

void AssetStreamer::Configure(const Config& config)
{
    m_impl->config = config;
    if (m_impl->config.maxJobsInFlight == 0) m_impl->config.maxJobsInFlight = 1;
    if (m_impl->config.maxUploadsPerPump == 0) m_impl->config.maxUploadsPerPump = 1;
}

const AssetStreamer::Config& AssetStreamer::GetConfig() const
{
    return m_impl->config;
}

void AssetStreamer::ResetForProjectSwitch()
{
    Impl& impl = *m_impl;
    /// @note 受付を止めてから epoch を進める。実行中ジョブは走りきって完了キューへ積むが、epoch が
    ///       一致しないので次の Pump で捨てられる (jobsInFlight はそこで戻る)。
    ++impl.epoch;
    for (auto& [id, entry] : impl.entries) impl.DiscardCandidate(*entry);
    impl.entries.clear();
    impl.entryByKey.clear();
    impl.entryByHandle.clear();
    impl.leases.clear();
    impl.channels.clear();
    impl.reportedSyncMisses.clear();
}

void AssetStreamer::NotifySyncFilled(std::string_view typeName, const std::string& key)
{
    Impl::Entry* entry = m_impl->FindByKey(typeName, key);
    if (!entry || Impl::IsPublished(*entry)) return;
    ++m_impl->syncMisses;
    if (m_impl->reportedSyncMisses.insert(JoinKey(typeName, key)).second)
        FBZZ_LOG_WARN("AssetStreamer: 非同期ロード中のアセットを同期 Load で待たずに読み直しました [%s]",
                      key.c_str());
    m_impl->AbandonWork(*entry);
    entry->failed = false;
    entry->canceled = false;
    entry->error = AssetLoadError::None;
    entry->message.clear();
    entry->publishedRevision = entry->requestedRevision;
}

void AssetStreamer::NotifyUnloaded(std::string_view typeName, const std::string& key)
{
    if (Impl::Entry* entry = m_impl->FindByKey(typeName, key)) {
        /// @note スロットは同期 Unload 側が返し済み。二重に Free しないよう先に外す。
        m_impl->entryByHandle.erase(HandleKey(entry->typeName, entry->handle));
        entry->handle = {};
        m_impl->DropEntry(*entry);
    }
}

AssetStreamer::Stats AssetStreamer::GetStats() const
{
    const Impl& impl = *m_impl;
    Stats stats;
    stats.entries = static_cast<std::uint32_t>(impl.entries.size());
    stats.jobsInFlight = impl.jobsInFlight;
    stats.staleCompletionsDropped = impl.staleCompletionsDropped;
    stats.syncMisses = impl.syncMisses;
    stats.syncLoads = impl.syncLoads;
    stats.uploadedBytesLastPump = impl.uploadedBytesLastPump;
    stats.residentBytes = impl.residentBytes;
    stats.evictions = impl.evictions;
    stats.deferredPrefetches = impl.deferredPrefetches;
    stats.qualityBias = impl.qualityBias;
    stats.overBudget = impl.overBudget;
    for (const auto& [id, entry] : impl.entries) {
        switch (Impl::StateOf(*entry)) {
        case AssetLoadState::Queued:              ++stats.queued; break;
        case AssetLoadState::WaitingDependencies: ++stats.waitingDependencies; break;
        case AssetLoadState::UploadQueued:
        case AssetLoadState::Uploading:           ++stats.uploading; break;
        case AssetLoadState::Ready:               ++stats.ready; break;
        case AssetLoadState::Failed:              ++stats.failed; break;
        default: break;
        }
        if (entry->decoded) stats.decodedBytesHeld += entry->decoded->CpuBytes();
    }
    return stats;
}

} // namespace fbzz::asset
