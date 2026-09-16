/// @file    VelocityFieldAtlas.cpp
/// @brief   .vfield を固定解像度タイルへリサンプルし、1 枚の Texture3D へ積む。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VelocityFieldAtlas.hpp>

#include <Engine/Asset/VectorFieldAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {
namespace {

constexpr uint32_t kTile      = VelocityFieldAtlas::kTileResolution;
constexpr size_t   kTileTexels = static_cast<size_t>(kTile) * kTile * kTile;

struct AtlasState {
    // 常駐表。キーは «その場の実体»。AssetStore のスロットは読み込み中は動かないので、
    // ポインタで引ける。プロジェクトを切り替えたら Reset() で丸ごと捨てる。
    std::unordered_map<const VectorFieldAsset*, int> tiles;
    // アトラス全体の CPU 側の写し。タイルが増えるたびにここから作り直す。
    std::vector<uint8_t> staging;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    uint32_t tileCount = 0;
    // 3D テクスチャを作れないバックエンドで毎フレーム試さないための一度きりの印。
    bool unsupported = false;

    // 作り直しで古くなったハンドル。**すぐには解放しない**。
    //
    // WHY 遅らせるか: ResourceManager::Release は即座に D3D オブジェクトを壊す。
    //     DX12 では前フレームのコマンドリストがまだそのテクスチャを読んでいる可能性があり、
    //     即時解放は use-after-free になる。数フレーム持ち越してから捨てる。
    struct Retired {
        renderer::ResourceHandle<renderer::TextureTag> texture;
        uint64_t frame;
    };
    std::vector<Retired> retired;
};

AtlasState& State()
{
    static AtlasState state;
    return state;
}

// 場を 1 タイルぶんのバイト列へ詰める。
// NormalizeVectorField が取り込み時に解像度も量子化も済ませているので、ここは詰め替えだけ。
// WHY ここでリサンプルしないか: «GPU に載せた瞬間に CPU の軌跡も変わる» を避けるため、
//     形を変える操作は取り込みの 1 か所に閉じてある。
void EncodeTile(const VectorFieldAsset& field, std::vector<uint8_t>& outTexels)
{
    outTexels.assign(kTileTexels * 4, 0);
    const size_t count = (std::min)(field.data.size(), kTileTexels);
    for (size_t i = 0; i < count; ++i) {
        outTexels[i * 4 + 0] = EncodeVectorFieldByte(field.data[i].x, field.maxMagnitude);
        outTexels[i * 4 + 1] = EncodeVectorFieldByte(field.data[i].y, field.maxMagnitude);
        outTexels[i * 4 + 2] = EncodeVectorFieldByte(field.data[i].z, field.maxMagnitude);
        outTexels[i * 4 + 3] = 255;
    }
}

// staging から Texture3D を作り直す。古いハンドルは retire へ回す。
void RebuildTexture(AtlasState& state, renderer::ResourceManager& resources)
{
    if (state.texture.IsValid())
        state.retired.push_back({ state.texture, Time::frameCount });

    const uint32_t depth = (std::max)(state.tileCount, 1u) * kTile;
    state.texture = resources.CreateTexture3D(state.staging.data(), kTile, kTile, depth);
    if (!state.texture.IsValid()) {
        state.unsupported = true;
        FBZZ_LOG_WARN("VelocityFieldAtlas: 3D テクスチャを作れません。"
                      "速度場は CPU シミュレーションでのみ効きます");
    }
}

// 数フレーム前に外したテクスチャを解放する。
void FlushRetired(AtlasState& state, renderer::ResourceManager& resources)
{
    // 3 フレーム。DX12 のフレームバッファリング (最大 2) より 1 つ多く取る。
    constexpr uint64_t kRetireDelay = 3;
    state.retired.erase(
        std::remove_if(state.retired.begin(), state.retired.end(),
                       [&](const AtlasState::Retired& entry) {
                           if (Time::frameCount < entry.frame + kRetireDelay) return false;
                           resources.Release(entry.texture);
                           return true;
                       }),
        state.retired.end());
}

} // namespace

int VelocityFieldAtlas::Acquire(const VectorFieldAsset& field, renderer::ResourceManager& resources)
{
    AtlasState& state = State();
    FlushRetired(state, resources);

    if (state.unsupported || field.Empty()) return -1;

    if (const auto found = state.tiles.find(&field); found != state.tiles.end())
        return found->second;

    if (state.tileCount >= kMaxTiles) {
        // 追い出しは実装しない。使われなくなった場を判定するには «誰が参照しているか» が
        // 要り、力場は毎フレーム組み直されるので «今フレーム使われた» しか分からない。
        // 64 枚を超える構成は演出の作り方のほうを見直すべき数字なので、はっきり落とす。
        FBZZ_LOG_WARN("VelocityFieldAtlas: 常駐できる速度場は %u 枚までです。"
                      "これ以上の場は効きません", kMaxTiles);
        return -1;
    }

    const int tile = static_cast<int>(state.tileCount);
    std::vector<uint8_t> texels;
    EncodeTile(field, texels);

    state.staging.resize(static_cast<size_t>(state.tileCount + 1) * kTileTexels * 4);
    std::copy(texels.begin(), texels.end(),
              state.staging.begin() + static_cast<std::ptrdiff_t>(
                  static_cast<size_t>(tile) * kTileTexels * 4));
    ++state.tileCount;

    RebuildTexture(state, resources);
    if (!state.texture.IsValid()) return -1;

    state.tiles.emplace(&field, tile);
    return tile;
}

renderer::ResourceHandle<renderer::TextureTag>
VelocityFieldAtlas::Texture(renderer::ResourceManager& resources)
{
    AtlasState& state = State();
    if (state.texture.IsValid()) return state.texture;

    // 場が 1 枚も無いときの 1x1x1。CS は tile < 0 で読まないので中身は何でもよいが、
    // **束縛されていること** が要る (DX12 の null ディスクリプタは Texture2D 固定で、
    // Texture3D を宣言したスロットを空にすると次元が食い違う)。
    if (!state.unsupported && state.tileCount == 0) {
        const uint8_t empty[4] = { 128, 128, 128, 255 }; // 復元すると 0 になる値
        state.texture = resources.CreateTexture3D(empty, 1, 1, 1);
        if (!state.texture.IsValid()) state.unsupported = true;
    }
    return state.texture;
}

void VelocityFieldAtlas::Invalidate(renderer::ResourceManager& resources)
{
    AtlasState& state = State();
    if (state.tiles.empty() && !state.texture.IsValid()) return;

    // テクスチャは «すぐには» 捨てない。前フレームのコマンドリストがまだ読んでいる
    // 可能性があり、即時解放は use-after-free になる (RebuildTexture と同じ理由)。
    if (state.texture.IsValid())
        state.retired.push_back({ state.texture, Time::frameCount });
    FlushRetired(state, resources);

    state.tiles.clear();
    state.staging.clear();
    state.texture = {};
    state.tileCount = 0;
    // unsupported は «このバックエンドで 3D テクスチャを作れない» という事実で、
    // アセットの差し替えでは変わらない。落とすと毎回作り直しを試みることになる。
}

void VelocityFieldAtlas::Reset()
{
    AtlasState& state = State();
    // ここでは Release しない。プロジェクト切り替えは ResourceManager 側の
    // 破棄と同時に起きるので、ハンドルを手放すだけでよい。
    state.tiles.clear();
    state.staging.clear();
    state.retired.clear();
    state.texture = {};
    state.tileCount = 0;
    state.unsupported = false;
}

} // namespace fbzz::asset
