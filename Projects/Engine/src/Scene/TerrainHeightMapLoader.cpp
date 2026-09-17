/// @file    TerrainHeightMapLoader.cpp
/// @brief   DirectXTex で画像ファイルと heightData[] を相互変換する。
/// @author  Hasegawa Jin
/// @date    2026-06-06
///
/// @note 読み込み: 拡張子でローダーを選ぶ → R32_FLOAT へ変換 → 地形サイズへ三次補間 → 正規化モードで写す。
/// @note DirectXTex の WIC コーデックに ole32.lib (COM) が要る。
/// @see https://github.com/microsoft/DirectXTex/wiki/WIC-I-O-Functions (DirectXTex Wiki, "WIC I/O Functions")
#pragma comment(lib, "ole32.lib")
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <DirectXTex.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace fbzz::scene {

namespace {

/// @brief 呼び出しスレッドで COM を使える状態にする。
/// @return COM が使えるか。既に別モード (STA) で初期化済みの RPC_E_CHANGED_MODE も使える扱い。
/// @note CoUninitialize は呼ばない。DirectXTex は WIC ファクトリをプロセス全体でキャッシュするため、
///       最後の参照で COM を畳むと次回の呼び出しが解放済みのファクトリを掴む。
/// @see https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex (CoInitializeEx, Return value)
bool HeightMapEnsureCom()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

} // namespace

bool LoadHeightMapFromFile(
    const std::string& path,
    TerrainComponent&  terrain,
    bool               unipolar)
{
    /// @note ハイトマップも ".meta" サイドカー表記 / 生画像の両形式を受け付ける。
    std::string sourcePath;
    if (!asset::TexDescSerializer::ResolveSourcePath(path, sourcePath)) return false;
    const std::wstring wpath = util::StringUtils::ToWide(sourcePath);

    if (!HeightMapEnsureCom()) return false;

    DirectX::ScratchImage image;
    HRESULT hr;
    if (sourcePath.ends_with(".dds") || sourcePath.ends_with(".DDS"))
        hr = DirectX::LoadFromDDSFile(wpath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
    else if (sourcePath.ends_with(".tga") || sourcePath.ends_with(".TGA"))
        hr = DirectX::LoadFromTGAFile(wpath.c_str(), nullptr, image);
    else
        hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);

    if (FAILED(hr)) return false;

    /// @note グレースケール (R8/R16) もカラー (R8G8B8A8) も R チャンネルだけを高さとして使う。
    DirectX::ScratchImage converted;
    hr = DirectX::Convert(
        *image.GetImage(0, 0, 0),
        DXGI_FORMAT_R32_FLOAT,
        DirectX::TEX_FILTER_DEFAULT,
        DirectX::TEX_THRESHOLD_DEFAULT,
        converted);
    if (FAILED(hr)) return false;

    const DirectX::Image* src = converted.GetImage(0, 0, 0);
    DirectX::ScratchImage resized;
    const size_t targetW = static_cast<size_t>(terrain.columns);
    const size_t targetH = static_cast<size_t>(terrain.rows);
    if (src->width != targetW || src->height != targetH) {
        hr = DirectX::Resize(*src, targetW, targetH, DirectX::TEX_FILTER_CUBIC, resized);
        if (FAILED(hr)) return false;
        src = resized.GetImage(0, 0, 0);
    }

    const size_t count = targetW * targetH;
    terrain.heightData.resize(count);
    for (size_t z = 0; z < targetH; ++z) {
        const std::uint8_t* row = src->pixels + z * src->rowPitch;
        for (size_t x = 0; x < targetW; ++x) {
            float value = 0.0f;
            std::memcpy(&value, row + x * sizeof(float), sizeof(float));
            const float p = std::clamp(value, 0.0f, 1.0f);
            terrain.heightData[z * targetW + x] = unipolar ? p : (p * 2.0f - 1.0f);
        }
    }

    terrain.heightDirty   = true;
    terrain.colliderDirty = true;
    return true;
}

bool SaveHeightMapToFile(
    const std::string&      path,
    const TerrainComponent& terrain,
    bool                    unipolar)
{
    if (path.empty() || terrain.columns <= 0 || terrain.rows <= 0
        || terrain.heightData.size() != terrain.VertexCount())
        return false;

    const size_t width  = static_cast<size_t>(terrain.columns);
    const size_t height = static_cast<size_t>(terrain.rows);

    DirectX::ScratchImage image;
    if (FAILED(image.Initialize2D(DXGI_FORMAT_R16_UNORM, width, height, 1, 1))) return false;
    const DirectX::Image* dst = image.GetImage(0, 0, 0);
    if (!dst) return false;

    for (size_t z = 0; z < height; ++z) {
        std::uint8_t* row = dst->pixels + z * dst->rowPitch;
        for (size_t x = 0; x < width; ++x) {
            const float h = terrain.heightData[z * width + x];
            /// @note LoadHeightMapFromFile の逆写像: unipolar は p = h、bipolar は p = h * 0.5 + 0.5。
            const float p = std::clamp(unipolar ? h : h * 0.5f + 0.5f, 0.0f, 1.0f);
            const std::uint16_t value = static_cast<std::uint16_t>(std::lround(p * 65535.0f));
            std::memcpy(row + x * sizeof(std::uint16_t), &value, sizeof(std::uint16_t));
        }
    }

    const std::filesystem::path fsPath(util::StringUtils::ToWide(path));
    if (fsPath.has_parent_path())
        (void)util::FileSystem::EnsureParentDirectory(fsPath);

    if (!HeightMapEnsureCom()) return false;

    /// @note R16_UNORM は WIC の 16bppGray へ写り、PNG エンコーダーはそのまま 16 bit グレースケールで保存する。
    const HRESULT hr = DirectX::SaveToWICFile(
        *dst, DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
        fsPath.c_str());
    return SUCCEEDED(hr);
}

} // namespace fbzz::scene
