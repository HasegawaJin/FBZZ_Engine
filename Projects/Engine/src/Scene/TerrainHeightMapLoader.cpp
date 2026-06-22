// FBZZ Engine
// TerrainHeightMapLoader.cpp | fbzz::scene
// DirectXTex を使って画像ファイルを heightData[] に変換する。
//
// 処理フロー:
//   1. 拡張子で LoadFromDDSFile / LoadFromTGAFile / LoadFromWICFile を選択
//   2. R32_FLOAT に変換（グレースケール・カラー問わず R チャンネルを使用）
//   3. 地形サイズと異なる場合は三次補間でリサイズ
//   4. 画素値 [0, 1] を heightData の正規化モードに応じてマッピング
#pragma comment(lib, "ole32.lib")  // DirectXTex の WIC コーデックに必要
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <DirectXTex.h>
#include <Windows.h>
#include <algorithm>
#include <string>

namespace fbzz::scene {

bool LoadHeightMapFromFile(
    const std::string& path,
    TerrainComponent&  terrain,
    bool               unipolar)
{
    const std::wstring wpath = util::StringUtils::ToWide(path);

    // 1. 拡張子でローダーを選択
    DirectX::ScratchImage image;
    HRESULT hr;
    if (path.ends_with(".dds") || path.ends_with(".DDS"))
        hr = DirectX::LoadFromDDSFile(wpath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
    else if (path.ends_with(".tga") || path.ends_with(".TGA"))
        hr = DirectX::LoadFromTGAFile(wpath.c_str(), nullptr, image);
    else
        hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);

    if (FAILED(hr)) return false;

    // 2. R32_FLOAT に変換する。
    //    グレースケール PNG (R8_UNORM / R16_UNORM) もカラー PNG (R8G8B8A8) も
    //    R チャンネルをそのまま使用する。ハイトマップは通常グレースケールで用意するため
    //    R=G=B が等しく、R チャンネルだけ取れば十分。
    DirectX::ScratchImage converted;
    hr = DirectX::Convert(
        *image.GetImage(0, 0, 0),
        DXGI_FORMAT_R32_FLOAT,
        DirectX::TEX_FILTER_DEFAULT,
        DirectX::TEX_THRESHOLD_DEFAULT,
        converted);
    if (FAILED(hr)) return false;

    // 3. 地形サイズに合わせてリサイズ（三次補間）
    const DirectX::Image* src = converted.GetImage(0, 0, 0);
    DirectX::ScratchImage resized;
    const size_t targetW = static_cast<size_t>(terrain.columns);
    const size_t targetH = static_cast<size_t>(terrain.rows);
    if (src->width != targetW || src->height != targetH) {
        hr = DirectX::Resize(*src, targetW, targetH, DirectX::TEX_FILTER_CUBIC, resized);
        if (FAILED(hr)) return false;
        src = resized.GetImage(0, 0, 0);
    }

    // 4. 画素値 → heightData にマッピング
    const float* pixels = reinterpret_cast<const float*>(src->pixels);
    const size_t count  = targetW * targetH;
    terrain.heightData.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const float p = std::clamp(pixels[i], 0.0f, 1.0f);
        terrain.heightData[i] = unipolar ? p : (p * 2.0f - 1.0f);
    }

    terrain.heightDirty   = true;
    terrain.colliderDirty = true;
    return true;
}

} // namespace fbzz::scene
