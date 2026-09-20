/// @file    RenderTargetCapture.cpp
/// @brief   ScratchImage → PNG (WIC) 変換の共通実装。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include "RenderTargetCapture.hpp"

#include <DirectXTex.h>

#include <cstring>

/// @note DirectXTex/WIC が内部で CoCreateInstance を呼ぶため ole32.lib をリンクする。
#pragma comment(lib, "ole32.lib")

namespace fbzz::renderer::detail {

bool EncodeCapturedImageToPng(const DirectX::ScratchImage& captured,
                              std::vector<std::uint8_t>& outPng,
                              std::uint32_t& outWidth, std::uint32_t& outHeight)
{
    const DirectX::Image* source = captured.GetImage(0, 0, 0);
    if (source == nullptr) return false;

    /// @note PNG は 8bit 前提。Scene View RT は HDR (R16G16B16A16_FLOAT) なので UNORM へ変換する。
    /// @note       変換は [0,1] へクランプする (トーンマップは行わない ― AI へのプレビュー用途として十分)。
    DirectX::ScratchImage converted;
    const DirectX::Image* toSave = source;
    const bool alreadyByte = source->format == DXGI_FORMAT_R8G8B8A8_UNORM
                          || source->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                          || source->format == DXGI_FORMAT_B8G8R8A8_UNORM
                          || source->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!alreadyByte) {
        const HRESULT hr = DirectX::Convert(*source, DXGI_FORMAT_R8G8B8A8_UNORM,
                                            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return false;
        toSave = converted.GetImage(0, 0, 0);
        if (toSave == nullptr) return false;
    }

    DirectX::Blob blob;
    const HRESULT hr = DirectX::SaveToWICMemory(*toSave, DirectX::WIC_FLAGS_NONE,
                                                DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), blob);
    if (FAILED(hr)) return false;

    const auto* bytes = static_cast<const std::uint8_t*>(blob.GetBufferPointer());
    outPng.assign(bytes, bytes + blob.GetBufferSize());
    outWidth  = static_cast<std::uint32_t>(toSave->width);
    outHeight = static_cast<std::uint32_t>(toSave->height);
    return true;
}

bool ReadCapturedImageAsLinearRGBA(const DirectX::ScratchImage& captured,
                                   std::vector<float>& outRgba,
                                   std::uint32_t& outWidth, std::uint32_t& outHeight)
{
    const DirectX::Image* source = captured.GetImage(0, 0, 0);
    if (source == nullptr) return false;

    /// @note R32G32B32A32_FLOAT へ正規化してから読む。DirectX::Convert は sRGB 形式なら
    /// @note       線形へ戻すので、バックエンドや RT 形式が変わっても指標の意味が揺れない。
    DirectX::ScratchImage converted;
    const DirectX::Image* toRead = source;
    if (source->format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
        const HRESULT hr = DirectX::Convert(*source, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return false;
        toRead = converted.GetImage(0, 0, 0);
        if (toRead == nullptr) return false;
    }

    const auto width  = static_cast<std::uint32_t>(toRead->width);
    const auto height = static_cast<std::uint32_t>(toRead->height);
    if (width == 0 || height == 0 || toRead->pixels == nullptr) return false;

    outRgba.resize(static_cast<std::size_t>(width) * height * 4u);
    /// @note rowPitch はアライメントのため width*16 より大きいことがある。行ごとにコピーする。
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* row = toRead->pixels + static_cast<std::size_t>(y) * toRead->rowPitch;
        std::memcpy(outRgba.data() + static_cast<std::size_t>(y) * width * 4u, row,
                    static_cast<std::size_t>(width) * 4u * sizeof(float));
    }
    outWidth  = width;
    outHeight = height;
    return true;
}

} /// @note namespace fbzz::renderer::detail
