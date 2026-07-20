// FBZZ Engine
// RenderTargetCapture.cpp | fbzz::renderer::detail
// ScratchImage → PNG (WIC) 変換の共通実装。
#include "RenderTargetCapture.hpp"

#include <DirectXTex.h>

#pragma comment(lib, "ole32.lib") // DirectXTex/WIC が内部で CoCreateInstance を呼ぶため

namespace fbzz::renderer::detail {

bool EncodeCapturedImageToPng(const DirectX::ScratchImage& captured,
                              std::vector<std::uint8_t>& outPng,
                              std::uint32_t& outWidth, std::uint32_t& outHeight)
{
    const DirectX::Image* source = captured.GetImage(0, 0, 0);
    if (source == nullptr) return false;

    // PNG は 8bit 前提。Scene View RT は HDR (R16G16B16A16_FLOAT) なので UNORM へ変換する。
    // 変換は [0,1] へクランプする (トーンマップは行わない ― AI へのプレビュー用途として十分)。
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

} // namespace fbzz::renderer::detail
