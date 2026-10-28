/// @file    PreparedSkinning.hpp
/// @brief   捕捉済み入力の閉じた DIRECT 計算列を必要になるまで投入せず保持する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <memory>
#include <span>

#if defined(_WIN32) && defined(FBZZGraphics_EXPORTS)
#define FBZZ_PREPARED_SKINNING_API __declspec(dllexport)
#elif defined(_WIN32)
#define FBZZ_PREPARED_SKINNING_API __declspec(dllimport)
#else
#define FBZZ_PREPARED_SKINNING_API
#endif

namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
namespace prepared_skinning_detail { class NativeWork; struct TicketFactory; }

/// @note source は元の SkinnedVertex バッファ。output は空を渡し、内部 pool の専用 Vertex 出力を取得する。
/// @note sourceVersion は native 内容版であり、CPU 転送要求の受理版ではない。
struct CapturedSkinningRequest {
    ResourceHandle<BufferTag> sourceVertices;
    ResourceHandle<BufferTag> outputVertices;
    uint64_t sourceVersion = 0;
    uint32_t vertexCount = 0, boneOffset = 0, boneCount = 0;
};

/// @note recorded は専用 DIRECT 列を投入したことを示す。GPU fence 完了を示さない。
struct PreparedSkinningWrite {
    ResourceHandle<BufferTag> output;
    uint64_t previousVersion = 0, resultingVersion = 0;
    bool recorded = false;
    uint32_t vertexCount = 0;
};

/// @note 元 pose/source/PSO/descriptor/output/allocator の寿命を持ち、Scene や Component を借用しない。
/// @note 同一 ticket の Submit と結果の公開は直列に呼ぶ。外部 callback の直前にも呼べ、投入は一度だけ。
class PreparedSkinningTicket final {
    struct ConstructionKey {};
public:
    explicit PreparedSkinningTicket(ConstructionKey, std::unique_ptr<prepared_skinning_detail::NativeWork> native);
    FBZZ_PREPARED_SKINNING_API ~PreparedSkinningTicket();
    PreparedSkinningTicket(const PreparedSkinningTicket&) = delete;
    PreparedSkinningTicket& operator=(const PreparedSkinningTicket&) = delete;
    /// @note 外部 callback、Logger、ResourceManager、資源確保、CPU/GPU 待機を呼ばない。
    [[nodiscard]] FBZZ_PREPARED_SKINNING_API bool Submit() noexcept;
    [[nodiscard]] FBZZ_PREPARED_SKINNING_API std::span<const PreparedSkinningWrite> Writes() const noexcept;
    /// @note 成功した row の現在内容版と native lease だけを照合する。未投入・外部版変更・旧 device は false。
    [[nodiscard]] FBZZ_PREPARED_SKINNING_API bool IsWriteCurrent(uint32_t row) const noexcept;
    [[nodiscard]] FBZZ_PREPARED_SKINNING_API uint64_t ResourceInstance() const noexcept;
    [[nodiscard]] FBZZ_PREPARED_SKINNING_API uint64_t ResourceEpoch() const noexcept;
    [[nodiscard]] FBZZ_PREPARED_SKINNING_API uint64_t ResourceFrame() const noexcept;
private:
    friend struct prepared_skinning_detail::TicketFactory;
    std::unique_ptr<prepared_skinning_detail::NativeWork> m_native;
};

/// @note 通常の同期準備境界。失敗・未知 backend・上限超過は空を返し、呼出し元は元の全処理へ戻す。
/// @note 入力 span は呼出し中だけ借り、callback に渡さず最初に値へコピーする。戻った後に Scene の寿命と入力を再検証すること。
[[nodiscard]] FBZZ_PREPARED_SKINNING_API std::shared_ptr<PreparedSkinningTicket> PrepareCapturedSkinning(
    IRenderer& renderer, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
    std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices);
[[nodiscard]] FBZZ_PREPARED_SKINNING_API bool SubmitPreparedSkinning(const std::shared_ptr<PreparedSkinningTicket>& ticket) noexcept;

/// @note 呼出し元の捕捉より前に判定する。環境変数 FBZZ_GPU_SKINNING_DEMAND が明示的な 1 の場合だけ有効で、未設定は無効。
[[nodiscard]] FBZZ_PREPARED_SKINNING_API bool GpuSkinningDemandEnabled() noexcept;
/// @note 登録済み facade と所有 Manager だけを照合し、仮想呼出し・待機 pump・資源解決は行わない。
[[nodiscard]] FBZZ_PREPARED_SKINNING_API bool SupportsCapturedSkinning(const IRenderer& renderer, const ResourceManager& resources) noexcept;
} /// @note namespace fbzz::renderer
#undef FBZZ_PREPARED_SKINNING_API
