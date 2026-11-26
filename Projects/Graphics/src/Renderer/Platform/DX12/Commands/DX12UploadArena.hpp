/// @file    DX12UploadArena.hpp
/// @brief   フェンスで保護されたフレーム別の GPU Upload ページを線形割り当てする。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Graphics/Api.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12UploadArena final {
public:
    static constexpr uint32_t FRAME_COUNT = 2;
    static constexpr size_t MAX_BYTES_PER_FRAME = 128u * 1024u * 1024u;

    struct Allocation {
        uint8_t* cpu = nullptr; ///< 割り当て先の CPU 書き込みアドレス。失敗時は null。
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0; ///< 割り当て先の GPU 仮想アドレス。
        size_t size = 0; ///< 割り当てたバイト数。
        ID3D12Resource* resource = nullptr; ///< コピー元に使えるページの非所有参照。
        size_t offset = 0; ///< ページ先頭から割り当て先までのバイト数。
        explicit operator bool() const { return cpu != nullptr; }
    };

    struct FrameUsage {
        size_t residentPages = 0; ///< 未使用の再利用候補を含む、スロットが保持するページ数。
        size_t pagesUsed = 0; ///< 当該フレームで割り当てに使ったページ数。
        size_t committedBytes = 0; ///< スロットが保持する Upload バッファの容量の合計 [byte]。
        size_t usedBytes = 0; ///< 当該フレームの使用量 [byte]。ページ内のアラインメント余白を含む。
        size_t peakBytesUsed = 0; ///< 初期化から現在までの、このスロットのフレーム使用量の最大値 [byte]。
        size_t failedAllocations = 0; ///< 当該フレームの割り当て失敗回数。
        size_t failedRequestBytes = 0; ///< 当該フレームで失敗した要求サイズの合計 [byte]。表現上限で飽和する。
        size_t largestFailedRequestBytes = 0; ///< 当該フレームで失敗した最大の要求サイズ [byte]。
    };

    DX12UploadArena() = default;
    DX12UploadArena(const DX12UploadArena&) = delete;
    DX12UploadArena& operator=(const DX12UploadArena&) = delete;
    FBZZ_GRAPHICS_API ~DX12UploadArena();

    /// @brief 各フレームスロットに最初の Upload ページを確保する。
    /// @param[in] device Upload ページを作る Direct3D デバイス。
    /// @param[in] bytesPerFrame 最初のページの容量 [byte]。
    /// @param[in] maxBytesPerFrame 最初のページを含むスロット全体の上限 [byte]。最初の容量と同じなら追加しない。
    /// @return 全スロットの初期確保に成功した場合は true。
    FBZZ_GRAPHICS_API bool Initialize(ID3D12Device* device, size_t bytesPerFrame,
        size_t maxBytesPerFrame = MAX_BYTES_PER_FRAME);

    /// @brief 全ページのマップを解除して破棄する。
    /// @pre すべてのページを参照する GPU 処理が完了していること。
    FBZZ_GRAPHICS_API void Shutdown();

    /// @brief フェンスを待機済みのスロットを巻き戻し、新しいフレームの計測を始める。
    /// @param[in] frameIndex 巻き戻すフレームスロット番号。
    /// @pre 当該スロットを参照する GPU 処理が完了していること。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management Fence-Based Resource Management
    FBZZ_GRAPHICS_API void BeginFrame(uint32_t frameIndex);

    /// @brief 現在のスロットから連続した Upload 領域を確保する。
    /// @param[in] sizeBytes 必要なバイト数。0 は失敗する。
    /// @param[in] alignment GPU アドレスのアラインメント [byte]。64 KiB 以下の二の冪。
    /// @return 成功時の領域はスロット再利用まで有効。失敗時は空。
    FBZZ_GRAPHICS_API Allocation Allocate(size_t sizeBytes, size_t alignment);

    /// @brief Upload ページから既存バッファへの転送を記録する。
    /// @param[in,out] commands 転送を追記するコマンドリスト。
    /// @param[in,out] destination COPY_DEST 状態の転送先バッファ。記録したコピーが指定範囲を書き換える。
    /// @param[in] destinationOffset 転送先バッファ先頭からのバイト数。
    /// @param[in] data sizeBytes バイトを読み取れる入力。
    /// @param[in] sizeBytes 転送するバイト数。
    /// @return 転送元を保持してコピーを記録したら true。失敗時は何も記録しない。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copybufferregion CopyBufferRegion のバイト単位のオフセット。
    [[nodiscard]] FBZZ_GRAPHICS_API bool TryStageCopy(ID3D12GraphicsCommandList& commands, ID3D12Resource& destination,
        size_t destinationOffset, const void* data, size_t sizeBytes);

    /// @brief 現在の GPU アドレスを CPU 側で再利用できるか照合する世代を返す。
    /// @note スロットの開始や破棄で進む。ページを追加しても同じフレーム内のアドレスは保たれる。
    uint64_t GetEpoch() const { return m_epoch; }

    /// @brief スロットの容量、使用量と失敗回数を描画スレッドで取得する。
    /// @param[in] frameIndex 照会するフレームスロット番号。0 から FRAME_COUNT 未満。
    /// @return 現在または最後に記録した使用状況。未初期化や範囲外は空。
    [[nodiscard]] FBZZ_GRAPHICS_API FrameUsage GetFrameUsage(uint32_t frameIndex) const;

private:
    static constexpr size_t MAX_SPILL_PAGES = 16;

    struct Page {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< このページの Upload バッファ。
        uint8_t* mapped = nullptr; ///< ページ先頭の永続マップ済み CPU アドレス。
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0; ///< ページ先頭の GPU 仮想アドレス。
        size_t capacity = 0; ///< ページのバッファ容量 [byte]。
        size_t offset = 0; ///< 次に割り当てる位置のページ先頭からのバイト数。
    };

    struct FrameArena {
        std::array<Page, 1 + MAX_SPILL_PAGES> pages; ///< 最初の固定ページと、必要に応じて確保する追加ページ。
        FrameUsage usage; ///< スロットの保持容量と直近のフレームの計測値。
        bool failureLogged = false; ///< 現在のフレームで直接割り当ての失敗を記録済みか。
    };

    bool CreatePage(Page& page, size_t bytes) const;
    static void ReleasePage(Page& page);
    static Allocation AllocateFromPage(FrameArena& frame, Page& page, size_t sizeBytes, size_t alignment);
    bool FreeUnusedPages(FrameArena& frame, size_t sizeBytes);
    Allocation TryAllocate(FrameArena& frame, size_t sizeBytes, size_t alignment);
    Allocation Allocate(size_t sizeBytes, size_t alignment, bool logFailure);

    std::array<FrameArena, FRAME_COUNT> m_frames; ///< 同時に進行中のフレームが別々に使用するスロット。
    Microsoft::WRL::ComPtr<ID3D12Device> m_device; ///< スロットに追加ページを確保するデバイス。
    size_t m_pageBytes = 0; ///< 最初のページと標準の追加ページの容量 [byte]。
    size_t m_maxBytesPerFrame = 0; ///< 各スロットが保持できるページ容量の合計上限 [byte]。
    uint32_t m_currentFrame = 0; ///< いま記録中のスロットの添字。
    uint64_t m_epoch = 0; ///< CPU 側の再利用アドレスを照合する世代。
};

} // namespace fbzz::renderer
