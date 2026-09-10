/// @file    VelocityFieldAtlas.hpp
/// @brief   常駐中の .vfield を 1 枚の Texture3D へ束ねる。GPU シミュレーションの入口。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY アトラスにするか:
///   速度場は «力場 1 本ごとに別のテクスチャ» を要求する。cs_5_0 (DX11 バックエンド) には
///   テクスチャの配列が無く、bindless は SM6.6 以降なので DX12 でしか使えない。
///   固定解像度のタイルを 1 枚へ Z 方向に積めば、SRV 1 枚で任意の本数を引ける。
///
/// WHY 解像度を固定するか:
///   アトラスのタイルは同じ寸法でなければ添字が計算できない。取り込み時に
///   kTileResolution へリサンプルする。表現力は落ちるが、粒子の流れとして
///   32³ より細かい格子は画面に出ない (雲の Detail ノイズと同じ結論)。
///   同時に «焼きすぎて 128³ が何枚も常駐する» という予算事故も防げる。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::asset {

struct VectorFieldAsset;

class VelocityFieldAtlas {
public:
    /// 1 タイルの 1 辺 [テクセル]。
    static constexpr uint32_t kTileResolution = 32;
    /// 常駐できる場の枚数。D3D11 の 3D テクスチャは 1 辺 2048 まで = 64 タイルが上限。
    /// 32³ × 64 枚 × RGBA8 = 8 MB。
    static constexpr uint32_t kMaxTiles = 64;

    /// 場をアトラスへ常駐させ、タイル番号を返す。既に載っていれば載っている番号。
    /// 満杯・3D テクスチャ非対応・空の場では -1 を返し、呼び出し側は «効かない力» として扱う。
    [[nodiscard]] static int Acquire(const VectorFieldAsset& field,
                                     renderer::ResourceManager& resources);

    /// CS へ束縛する Texture3D。**常に有効なハンドルを返す**。
    /// WHY 空でもダミーを返すか: DX12 の null ディスクリプタは Texture2D 固定で、
    ///     Texture3D を宣言したスロットを未束縛にすると次元が食い違う。
    ///     デバッグレイヤーが警告し、読み値も未定義になる。1x1x1 を常に置く。
    [[nodiscard]] static renderer::ResourceHandle<renderer::TextureTag>
        Texture(renderer::ResourceManager& resources);

    /// アトラスの Z 方向のテクセル数 (= kTileResolution * kMaxTiles)。
    /// シェーダーがタイル番号を UV へ直すのに使う。
    [[nodiscard]] static constexpr uint32_t AtlasDepth() { return kTileResolution * kMaxTiles; }

    /// プロジェクト切り替えで捨てる。常駐表とテクスチャの両方を手放す。
    static void Reset();
};

} // namespace fbzz::asset
