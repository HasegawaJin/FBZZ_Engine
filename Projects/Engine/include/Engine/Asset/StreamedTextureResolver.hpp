/// @file    StreamedTextureResolver.hpp
/// @brief   描画側が毎フレーム引くテクスチャ参照を、AssetStreamer の利用権へ結ぶ解決口。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::asset {

struct TextureAsset;

/// @brief 参照ごとに利用権を 1 つ持ち、使われなくなったら手放す。
/// @note 描画側は毎フレーム Resolve を呼ぶだけ。手放した後の解放は AssetStreamer の猶予と予算が決める。
/// @note メインスレッド専用。
/// @see Docs/design/asset-streaming.md «常駐予算とストリーミング»
class StreamedTextureResolver {
public:
    /// @brief 未完成のテクスチャを引かれたときの振る舞い。
    enum class MissPolicy : std::uint8_t {
        /// その場で同期ロードして返す。既存の LoadTexture と同じ絵になる (既定)。
        Synchronous,
        /// 無効ハンドルを返し、完成したフレームから使わせる (マテリアルはテクスチャ無しで描く)。
        Placeholder,
    };

    [[nodiscard]] static StreamedTextureResolver& Engine();

    /// @brief 参照 (パス・guid 参照・Sprite 参照) の GPU 実体を返す。
    /// @param screenPixels このテクスチャを貼って描く物体の画面上の直径 [px]。0 以下は «不明» (最高品質を保つ)。
    /// @return 非同期経路が使えない (AssetManager 未初期化・予算超過で拒否) ときは従来の LoadTexture を返す。
    [[nodiscard]] renderer::ResourceHandle<renderer::TextureTag> ResolveGpu(
        renderer::ResourceManager& resources, std::string_view reference, float screenPixels = 0.0f);

    /// @brief 画面上の大きさから品質段を決める。テクスチャの長辺が画面の何倍かを 2 の対数で段にし、1 段の余裕を残す。
    /// @return 0 (最高品質) 以上 lowest 以下。screenPixels が 0 以下なら 0。
    [[nodiscard]] static AssetQuality QualityForScreenSize(uint32_t textureWidth, uint32_t textureHeight,
                                                           float screenPixels, AssetQuality lowest);

    /// @brief 画面上の大きさによる品質の自動選択を有効にする。既定は無効 (既存の絵を変えないため)。
    /// @note 有効にしても、切り替えは台帳のヒステリシス (qualityHoldPumps) を経てから起きる。
    void SetAutoQuality(bool enabled) { m_autoQuality = enabled; }
    [[nodiscard]] bool GetAutoQuality() const { return m_autoQuality; }

    /// @brief 公開済みの TextureAsset。未完成・失敗なら nullptr。元画像寸法 (sourceWidth) を引く用途。
    [[nodiscard]] const TextureAsset* ResolveAsset(renderer::ResourceManager& resources, std::string_view reference);

    /// @brief ResolveGpu と同時に元画像の寸法を返す (スプライト矩形の UV 換算用)。
    /// @note 品質段で縮小して常駐していても元の寸法を返す。分からなければ GPU 実体の寸法、それも無ければ 0。
    [[nodiscard]] renderer::ResourceHandle<renderer::TextureTag> ResolveGpuWithSourceSize(
        renderer::ResourceManager& resources, std::string_view reference,
        uint32_t& outSourceWidth, uint32_t& outSourceHeight);

    /// @brief フレームの終わりに呼ぶ。releaseAfterFrames の間引かれなかった参照の利用権を手放す。
    void EndFrame();
    /// @brief 全利用権を手放す (プロジェクト切り替え)。
    void Reset();

    void SetMissPolicy(MissPolicy policy) { m_missPolicy = policy; }
    [[nodiscard]] MissPolicy GetMissPolicy() const { return m_missPolicy; }
    void SetReleaseAfterFrames(std::uint32_t frames) { m_releaseAfterFrames = frames; }
    [[nodiscard]] std::size_t HeldCount() const { return m_entries.size(); }

private:
    struct Entry {
        AssetLease<TextureAsset> lease;
        std::uint64_t            lastUsedFrame = 0;
        /// 同期ロードに一度失敗した。非同期の結果 (失敗理由付き) を待ち、毎フレーム読み直さない。
        bool                     syncFailed = false;
        /// このフレームで引かれた中で最も大きい画面上の直径 [px]。0 は不明。
        float                    maxScreenPixels = 0.0f;
        bool                     screenSizeKnown = true;
        AssetQuality             appliedQuality = 0;
    };

    /// @return 非同期経路で扱えないなら nullptr。
    Entry* Acquire(std::string_view reference);
    const TextureAsset* Complete(Entry& entry);

    std::unordered_map<std::string, Entry> m_entries;
    std::uint64_t m_frame = 0;
    std::uint32_t m_releaseAfterFrames = 300;
    MissPolicy    m_missPolicy = MissPolicy::Synchronous;
    bool          m_autoQuality = false;
};

} // namespace fbzz::asset
