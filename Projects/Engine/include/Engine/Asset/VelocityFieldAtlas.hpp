/// @file    VelocityFieldAtlas.hpp
/// @brief   常駐中の 速度場 PNG を 1 枚の Texture3D へ束ねる。GPU シミュレーションの入口。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note アトラス化の理由: cs_5_0 (DX11) はテクスチャ配列を持たず、bindless は DX12 限定の SM6.6 以降の
///       ため、固定寸法タイルを Z 方向に積んで SRV 1 枚で束ねる。
/// @note 解像度を kTileResolution へ固定するのは添字計算のため。32³ より細かい格子は画面に出ず (雲の
///       Detail ノイズと同じ)、128³ が何枚も常駐する予算事故も防げる。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::fluid { struct VectorFieldAsset; }

namespace fbzz::asset {

class VelocityFieldAtlas {
public:
    /// @brief 1 タイルの 1 辺 [テクセル]。
    static constexpr uint32_t kTileResolution = 32;
    /// @brief 常駐できる場の枚数。D3D11 の 3D テクスチャは 1 辺 2048 まで = 64 タイルが上限。
    /// @brief 32³ × 64 枚 × RGBA8 = 8 MB。
    static constexpr uint32_t kMaxTiles = 64;

    /// @brief 場をアトラスへ常駐させ、タイル番号を返す。既に載っていれば載っている番号。
    /// @brief 満杯・3D テクスチャ非対応・空の場では -1 を返し、呼び出し側は «効かない力» として扱う。
    [[nodiscard]] static int Acquire(const fluid::VectorFieldAsset& field,
                                     renderer::ResourceManager& resources);

    /// @brief CS へ束縛する Texture3D。**常に有効なハンドルを返す**。
    /// @note DX12 の null ディスクリプタは Texture2D 固定のため、Texture3D スロットを未束縛にすると
    ///       次元が食い違い検証レイヤーが警告し、読み値も未定義になる。常に 1x1x1 を返す。
    [[nodiscard]] static renderer::ResourceHandle<renderer::TextureTag>
        Texture(renderer::ResourceManager& resources);

    /// @brief アトラスの Z 方向のテクセル数 (= kTileResolution * kMaxTiles)。
    /// @brief シェーダーがタイル番号を UV へ直すのに使う。
    [[nodiscard]] static constexpr uint32_t AtlasDepth() { return kTileResolution * kMaxTiles; }

    /// @brief 常駐表を捨てて、次の Acquire で焼き直させる。
    /// @note 常駐表はアセットの実体をポインタで指す。速度場 PNG のホットリロードは AssetStore の
    ///       スロットへ新しい実体を差し込むため、放置すると古いポインタを指したタイルが食い潰され、
    ///       粒子は古い場を読み続ける。1 枚だけの焼き直しには対応表が要るため、64 枚まとめて焼く。
    static void Invalidate(renderer::ResourceManager& resources);

    /// @brief プロジェクト切り替えで捨てる。常駐表とテクスチャの両方を手放す。
    /// @note ResourceManager ごと破棄される前提なので、テクスチャは解放しない。
    static void Reset();
};

} // namespace fbzz::asset
