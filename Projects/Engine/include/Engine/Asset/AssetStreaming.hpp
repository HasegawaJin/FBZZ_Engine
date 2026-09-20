/// @file    AssetStreaming.hpp
/// @brief   アセットの非同期要求・利用権・フレーム境界での公開。
/// @author  Hasegawa Jin
/// @date    2026-09-19
///
/// ファイル読み込みとデコードをワーカーへ、GPU 転送の投入と完成物の公開をメインスレッドの
/// Pump() へ分ける。ストア (AssetStore<T>) はメインスレッド所有のまま、完成した結果だけを反映する。
/// @see Docs/design/asset-streaming.md
#pragma once
#include <Engine/Asset/AssetHandle.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace fbzz::asset {

/// @brief ロード段階。READING と DECODING はワーカー 1 回の呼び出しで行うため Decoding に畳む。
enum class AssetLoadState : std::uint8_t {
    None,                ///< 知らないハンドル (未要求・解放済み・プロジェクト切り替え前のもの)
    Queued,
    Decoding,
    WaitingDependencies,
    UploadQueued,
    Uploading,
    Ready,
    Failed,
    Canceled,
};

enum class AssetLoadError : std::uint8_t {
    None,
    NoChannel,           ///< その型の非同期経路が登録されていない
    NotFound,            ///< 参照を実ファイルへ解決できない
    DecodeFailed,
    UploadFailed,
    DependencyFailed,
    DependencyCycle,
    BudgetExceeded,      ///< 台帳の枠 (maxEntries) が尽きた、または固定物だけで常駐予算を超えている
    ShuttingDown,
};

/// @brief 要求の優先度。小さいほど先に投入する。同順位は待ったフレーム数の長い順。
enum class AssetPriority : std::uint8_t { Required = 0, Visible = 1, Prefetch = 2, Background = 3 };

/// @brief 品質段。0 が最高で、1 段ごとに解像度 (テクスチャは mip、モデルは LOD) を 1 段落とす。
using AssetQuality = std::uint8_t;

struct AssetRequestOptions {
    AssetPriority priority = AssetPriority::Visible;
    /// 希望品質。同じアセットの利用権の中で最も高い (小さい) 値に集約する。
    AssetQuality  quality = 0;
};

struct AssetLoadStatus {
    AssetLoadState state = AssetLoadState::None;
    AssetLoadError error = AssetLoadError::None;
    /// 公開中の中身の世代。0 は未公開。ホットリロードと品質変更で進む。
    std::uint32_t  publishedRevision = 0;
    /// 公開中の実体はそのまま、再読み込みの候補だけが失敗した。
    bool           updateFailed = false;
    /// 実際に常駐している品質。希望品質とは別 (ヒステリシスと予算で遅れる)。
    AssetQuality   residentQuality = 0;
    AssetQuality   desiredQuality = 0;
    /// 公開中の実体が占めるバイト数 (経路の見積もり)。
    std::uint64_t  residentBytes = 0;
    std::string    message;
};

/// @brief ストアの型に依らないハンドル。AssetHandle<T> と同じ値を持つ。
struct RawAssetHandle {
    std::uint32_t id  = 0;
    std::uint32_t gen = 0;
    [[nodiscard]] bool IsValid() const { return id != 0; }
    bool operator==(const RawAssetHandle&) const = default;
};

/// @brief 親が読み込みの途中で要求する別アセット。
struct AssetDependency {
    std::string typeName;   ///< AssetStreamTypeName<T>() の値
    std::string reference;
    /// false の依存は失敗しても親を止めず、完成も待たない (型別の代替表示に任せる)。
    bool        required = true;
};

/// @brief ワーカーが作る GPU 非依存の成果物。
/// @note GPU ハンドルや Scene を指すものを入れない。ワーカーとメインスレッドを跨いで move される。
struct IDecodedAsset {
    virtual ~IDecodedAsset() = default;
    /// 転送量・台帳の計上に使うバイト数。
    [[nodiscard]] virtual std::size_t CpuBytes() const { return 0; }
    std::vector<AssetDependency> dependencies;
};

/// @brief メインスレッドで確定させ、ワーカーへ読み取り専用で渡す型別の入力 (import 設定など)。
struct AssetJobContext {
    virtual ~AssetJobContext() = default;
};

/// @brief ワーカー 1 件分の入力。メインスレッドで作り、以後は書き換えない。
struct AssetJobInput {
    std::string                            key;
    std::string                            resolvedPath;
    std::shared_ptr<const AssetJobContext> context;
    /// 作る表現の品質段。品質を持たない経路は無視してよい。
    AssetQuality                           quality = 0;
};

struct AssetDecodeResult {
    std::unique_ptr<IDecodedAsset> decoded;
    AssetLoadError                 error = AssetLoadError::None;
    std::string                    message;
};

/// @brief GPU 転送中または公開待ちの完成候補。公開 (Publish) されるか捨てられる (Discard)。
struct IAssetCandidate {
    virtual ~IAssetCandidate() = default;
};

/// @brief 型 1 つ分の非同期経路。ストア操作・デコード・転送を AssetStreamer から隠す。
/// @note Decode 以外はメインスレッドから呼ぶ。Decode はワーカーから並行に呼ばれるので、
///       メンバーの可変状態・Logger・AssetDatabase・ストアに触らない。
class IAssetStreamChannel {
public:
    virtual ~IAssetStreamChannel() = default;

    [[nodiscard]] virtual std::string_view TypeName() const = 0;

    /// @name メインスレッド: ストア
    /// @{
    /// @brief 参照を重複集約のキーへ正規化する。同期 Load<T> のキャッシュキーと一致させる。
    [[nodiscard]] virtual std::string MakeKey(const std::string& reference) = 0;
    /// @brief ストアのキャッシュに載っているハンドル (予約中を含む)。無ければ無効値。
    [[nodiscard]] virtual RawAssetHandle Lookup(const std::string& key) = 0;
    /// @brief 空のスロットを予約してキャッシュへ載せる。同期 Load<T> も以後このハンドルを返す。
    [[nodiscard]] virtual RawAssetHandle Reserve(const std::string& key) = 0;
    [[nodiscard]] virtual bool IsLive(RawAssetHandle handle) const = 0;
    [[nodiscard]] virtual bool IsFilled(RawAssetHandle handle) const = 0;
    [[nodiscard]] virtual void* Get(RawAssetHandle handle) const = 0;
    /// @brief スロットを返して世代を進め、キャッシュから外す。以後そのハンドルは無効。
    virtual void Free(RawAssetHandle handle, const std::string& key) = 0;
    /// @brief 利用権の無くなった公開済みアセットを常駐から外す。
    /// @return 外せない (同期 Load が固定している・GPU 実体が他で使われている) なら false。
    [[nodiscard]] virtual bool Evict(RawAssetHandle handle, const std::string& key) = 0;
    /// @brief 公開中の実体が占めるバイト数の見積もり。非同期で作った実体は台帳が CPU 成果物の大きさで数える。
    [[nodiscard]] virtual std::uint64_t EstimateResidentBytes(RawAssetHandle /*handle*/) const { return 0; }
    /// @brief 予約中のスロットをこの場で同期ロードして埋める。待機を許した箇所 (ロード画面など) だけが使う。
    /// @return 経路が同期ロードを持たない、または失敗したら false。
    [[nodiscard]] virtual bool LoadImmediately(RawAssetHandle /*handle*/, const std::string& /*key*/) { return false; }
    /// @}

    /// @name 品質段
    /// @{
    /// @brief 品質段を持つ経路か。持たなければ台帳は希望品質を無視する。
    [[nodiscard]] virtual bool SupportsQuality() const { return false; }
    /// @brief 落とせる最低品質の段。
    [[nodiscard]] virtual AssetQuality LowestQuality() const { return 0; }
    /// @}

    /// @brief ワーカーへ渡す入力を確定させる (パス解決・import 設定の読み取り)。
    /// @return 解決できなければ false と理由。
    [[nodiscard]] virtual bool Snapshot(const std::string& key, AssetJobInput& out, std::string& outError) = 0;

    /// @brief ワーカースレッド: 確定済みの入力からファイルを読んで CPU 成果物を作る。
    [[nodiscard]] virtual AssetDecodeResult Decode(const AssetJobInput& input) = 0;

    /// @name メインスレッド: GPU 転送と公開
    /// @{
    /// @brief GPU 成果物の世代。変わったら転送中の候補は捨てて CPU 成果物から作り直す。
    [[nodiscard]] virtual std::uint64_t DeviceEpoch() const { return 0; }
    /// @brief 候補を作って転送を投入する。CPU だけの型はここで完成物を作り token 0 を返す。
    /// @return 失敗なら nullptr と理由。
    [[nodiscard]] virtual std::unique_ptr<IAssetCandidate> BeginUpload(
        IDecodedAsset& decoded, std::uint64_t& outToken, std::string& outError) = 0;
    [[nodiscard]] virtual bool IsUploadComplete(std::uint64_t /*token*/) const { return true; }
    /// @brief 完成した候補をスロットへ入れる。旧実体はここで置き換わる。
    /// @return スロットが既に無効なら false (候補は Discard される)。
    [[nodiscard]] virtual bool Publish(RawAssetHandle handle, IAssetCandidate& candidate) = 0;
    /// @brief 公開しない候補を捨てる。
    /// @param deviceStillValid false ならデバイスは作り直された後で、GPU 実体はもう返却済み。
    virtual void Discard(IAssetCandidate& candidate, bool deviceStillValid) = 0;
    /// @}
};

/// @brief 型の識別名。チャンネルと要求側で同じ値を使う。
/// @note typeid の名前は DLL と EXE で同じ文字列になるので、境界を跨いでもチャンネルを引ける。
template<typename T>
[[nodiscard]] std::string_view AssetStreamTypeName()
{
    return typeid(T).name();
}

class AssetStreamer;

/// @brief 要求 1 件分の利用権。move-only で、破棄または Release で自分の要求だけを解く。
/// @note CPU / GPU 実体は所有しない。生成・破棄・解放はメインスレッドで行う。
/// @note 実体が要るときは毎フレーム AssetStreamer::TryGet で引く。raw pointer をフレームを跨いで持たない。
template<typename T>
class AssetLease {
public:
    AssetLease() = default;
    AssetLease(AssetStreamer* owner, std::uint64_t leaseId, AssetHandle<T> handle, AssetLoadError error)
        : m_owner(owner), m_leaseId(leaseId), m_handle(handle), m_error(error) {}
    ~AssetLease() { Release(); }

    AssetLease(const AssetLease&) = delete;
    AssetLease& operator=(const AssetLease&) = delete;
    AssetLease(AssetLease&& other) noexcept { MoveFrom(other); }
    AssetLease& operator=(AssetLease&& other) noexcept
    {
        if (this != &other) { Release(); MoveFrom(other); }
        return *this;
    }

    /// @brief 予約されたハンドル。IsValid() は «ロード完了» ではない。
    [[nodiscard]] AssetHandle<T>  Handle() const { return m_handle; }
    /// @brief 受付時点で拒否された理由 (枠不足・経路未登録など)。受け付けたなら None。
    [[nodiscard]] AssetLoadError  RequestError() const { return m_error; }
    [[nodiscard]] bool            IsHeld() const { return m_leaseId != 0; }

    void SetPriority(AssetPriority priority);
    /// @brief 希望品質を更新する。実際の常駐品質はヒステリシスを経て後から追いつく。
    void SetDesiredQuality(AssetQuality quality);
    void Release();

private:
    void MoveFrom(AssetLease& other)
    {
        m_owner = other.m_owner;
        m_leaseId = other.m_leaseId;
        m_handle = other.m_handle;
        m_error = other.m_error;
        other.m_owner = nullptr;
        other.m_leaseId = 0;
        other.m_handle = AssetHandle<T>::Null();
    }

    AssetStreamer* m_owner = nullptr;
    std::uint64_t  m_leaseId = 0;
    AssetHandle<T> m_handle;
    AssetLoadError m_error = AssetLoadError::None;
};

/// @brief 要求台帳。要求の重複集約・利用権・ワーカー投入・完了の検証と公開を持つ。
/// @note すべての公開メンバーはメインスレッドから呼ぶ。ワーカーが触るのは内部の完了キューだけ。
/// @note 受付から公開までの各結果は (projectEpoch, attempt, contentRevision, deviceEpoch) で照合し、
///       古いものは捨てる。
/// @see Docs/design/asset-streaming.md «状態と世代»
class AssetStreamer {
public:
    struct Config {
        /// 同時にワーカーへ出すジョブ数。完了キューの長さもこれで抑える。
        std::uint32_t maxJobsInFlight = 4;
        /// 1 回の Pump で投入する GPU 転送の件数とバイト数。
        std::uint32_t maxUploadsPerPump = 8;
        std::uint64_t maxUploadBytesPerPump = 64ull * 1024ull * 1024ull;
        /// 台帳が同時に持てる要求キーの数。
        std::uint32_t maxEntries = 16384;
        /// 常駐予算 (公開済みの実体 + 転送待ちの CPU 成果物)。0 は無制限。
        std::uint64_t residentBudgetBytes = 0;
        /// 利用権がゼロになってから解放するまでの猶予 (Pump 回数)。予算超過中は待たずに LRU で解放する。
        /// @note 0 は猶予による解放をしない (予算超過のときだけ解放する)。既定を 0 にしているのは、
        ///       解放した物を再び使うと同期ロードの引っかかりになり、既存ゲームの手触りが変わるため。
        std::uint32_t evictionGracePumps = 0;
        /// 希望品質がこの回数だけ続いてから常駐品質を動かす (ちらつき防止のヒステリシス)。
        std::uint32_t qualityHoldPumps = 30;
    };

    struct Stats {
        std::uint32_t entries = 0;
        std::uint32_t queued = 0;
        std::uint32_t jobsInFlight = 0;
        std::uint32_t waitingDependencies = 0;
        std::uint32_t uploading = 0;
        std::uint32_t ready = 0;
        std::uint32_t failed = 0;
        std::uint64_t staleCompletionsDropped = 0;
        std::uint64_t syncMisses = 0;
        std::uint64_t syncLoads = 0;
        std::uint64_t uploadedBytesLastPump = 0;
        std::uint64_t decodedBytesHeld = 0;
        std::uint64_t residentBytes = 0;
        std::uint64_t evictions = 0;
        std::uint32_t deferredPrefetches = 0;
        /// 予算超過で全体に上乗せしている品質の段数。
        AssetQuality  qualityBias = 0;
        bool          overBudget = false;
    };

    /// @brief ジョブ 1 件を実行させる関数。本番は TaskSystem、テストは手で回すキュー。
    using JobExecutor = std::function<void(std::function<void()>)>;

    /// @brief エンジン全体で共有する台帳 (TaskSystem で実行)。
    [[nodiscard]] static AssetStreamer& Engine();

    explicit AssetStreamer(JobExecutor executor);
    AssetStreamer(JobExecutor executor, Config config);
    ~AssetStreamer();
    AssetStreamer(const AssetStreamer&) = delete;
    AssetStreamer& operator=(const AssetStreamer&) = delete;

    /// @brief 型の経路を登録する。同じ型名は置き換える。
    void RegisterChannel(std::shared_ptr<IAssetStreamChannel> channel);

    /// @brief 予算・猶予などを差し替える。次の Pump から効く。
    void Configure(const Config& config);
    [[nodiscard]] const Config& GetConfig() const;

    /// @brief 非同期ロードを要求する。I/O を待たない。
    /// @return 受け付けたら予約済みハンドルを持つ利用権。拒否なら無効ハンドルと RequestError。
    template<typename T>
    [[nodiscard]] AssetLease<T> Request(const std::string& reference, const AssetRequestOptions& options = {})
    {
        const RawRequest raw = RequestRaw(AssetStreamTypeName<T>(), reference, options);
        return AssetLease<T>(raw.leaseId != 0 ? this : nullptr, raw.leaseId,
                             AssetHandle<T>{ raw.handle.id, raw.handle.gen }, raw.error);
    }

    template<typename T>
    [[nodiscard]] AssetLoadStatus GetStatus(AssetHandle<T> handle) const
    {
        return GetStatusRaw(AssetStreamTypeName<T>(), { handle.id, handle.gen });
    }

    /// @brief 公開済み (READY) の実体。未完成・失敗・解放済みなら nullptr。
    template<typename T>
    [[nodiscard]] T* TryGet(AssetHandle<T> handle) const
    {
        return static_cast<T*>(TryGetRaw(AssetStreamTypeName<T>(), { handle.id, handle.gen }));
    }

    /// @brief 失敗した要求をもう一度投入する。毎フレームの自動再試行はしない。
    template<typename T>
    bool Retry(AssetHandle<T> handle) { return RetryRaw(AssetStreamTypeName<T>(), { handle.id, handle.gen }); }

    /// @brief 公開中の実体を残したまま読み直す。候補が失敗しても旧実体を使い続ける。
    template<typename T>
    bool Reload(AssetHandle<T> handle) { return ReloadRaw(AssetStreamTypeName<T>(), { handle.id, handle.gen }); }

    /// @brief 未完成の要求をこの場で同期ロードして公開する。ロード画面など待機を許した箇所だけが使う。
    /// @return 公開済みになったら true。
    template<typename T>
    bool CompleteNow(AssetHandle<T> handle) { return CompleteNowRaw(AssetStreamTypeName<T>(), { handle.id, handle.gen }); }

    /// @brief フレーム境界の処理。完了の回収 → 依存の判定 → 転送の投入と完了確認 → 公開 → 次のジョブ投入。
    /// @note Scene 更新と描画コマンドの記録より前 (レンダラーのフレーム外) で呼ぶ。
    void Pump();

    /// @brief プロジェクト切り替え。epoch を進め、全要求と経路を捨てる。実行中ジョブの結果は棄却される。
    /// @note ResourceManager が生きているうちに呼ぶ (転送中の候補を返すため)。
    void ResetForProjectSwitch();

    /// @brief 同期 Load<T> が予約中のスロットを自分で埋めた。実行中の非同期結果を棄却する。
    void NotifySyncFilled(std::string_view typeName, const std::string& key);
    /// @brief 同期 Unload<T> がスロットを返した。台帳からも外す。
    void NotifyUnloaded(std::string_view typeName, const std::string& key);

    [[nodiscard]] Stats GetStats() const;

    /// @name 型を消した入口 (テンプレートと AssetLease が使う)
    /// @{
    struct RawRequest {
        std::uint64_t  leaseId = 0;
        RawAssetHandle handle;
        AssetLoadError error = AssetLoadError::None;
    };
    [[nodiscard]] RawRequest RequestRaw(std::string_view typeName, const std::string& reference,
                                        const AssetRequestOptions& options);
    [[nodiscard]] AssetLoadStatus GetStatusRaw(std::string_view typeName, RawAssetHandle handle) const;
    [[nodiscard]] void* TryGetRaw(std::string_view typeName, RawAssetHandle handle) const;
    bool RetryRaw(std::string_view typeName, RawAssetHandle handle);
    bool ReloadRaw(std::string_view typeName, RawAssetHandle handle);
    bool CompleteNowRaw(std::string_view typeName, RawAssetHandle handle);
    void ReleaseLease(std::uint64_t leaseId);
    void SetLeasePriority(std::uint64_t leaseId, AssetPriority priority);
    void SetLeaseQuality(std::uint64_t leaseId, AssetQuality quality);
    [[nodiscard]] AssetLoadStatus GetLeaseStatus(std::uint64_t leaseId) const;
    /// @}

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

template<typename T>
void AssetLease<T>::SetPriority(AssetPriority priority)
{
    if (m_owner) m_owner->SetLeasePriority(m_leaseId, priority);
}

template<typename T>
void AssetLease<T>::SetDesiredQuality(AssetQuality quality)
{
    if (m_owner) m_owner->SetLeaseQuality(m_leaseId, quality);
}

template<typename T>
void AssetLease<T>::Release()
{
    if (m_owner && m_leaseId != 0) m_owner->ReleaseLease(m_leaseId);
    m_owner = nullptr;
    m_leaseId = 0;
}

} // namespace fbzz::asset
