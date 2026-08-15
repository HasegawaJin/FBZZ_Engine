// FBZZ Engine
// RenderTargetCapture.hpp | fbzz::renderer::detail
// DX11 / DX12 が取得した DirectXTex ScratchImage を PNG バイト列へ変換する共通処理。
//
// WHY: RT 読み戻し自体はバックエンド固有 (CaptureTexture の引数が違う) だが、
//      「掴んだ画像を R8G8B8A8 化して PNG へエンコードする」後段は完全に共通なので1か所へ集約する。
//      本ヘッダは DirectXTex を include せず前方宣言のみで軽量に保つ (include は .cpp に閉じる)。
#pragma once
#include <cstdint>
#include <vector>

namespace DirectX { class ScratchImage; }

namespace fbzz::renderer::detail {

// captured (CaptureTexture の結果) の mip0/slice0 を PNG へエンコードして outPng へ書く。
// HDR (R16G16B16A16_FLOAT 等) は R8G8B8A8_UNORM へ変換してから保存する。失敗時 false。
bool EncodeCapturedImageToPng(const DirectX::ScratchImage& captured,
                              std::vector<std::uint8_t>& outPng,
                              std::uint32_t& outWidth, std::uint32_t& outHeight);

// captured の mip0/slice0 を線形 RGBA float 列 (画素あたり 4 要素・行優先) へ展開する。
// WHY: PNG 化は [0,1] へクランプするため、露出の破綻 (白飛び) と「単に明るい」を区別できない。
//      数値評価には HDR のままの線形値が必要なので、8bit 化と別経路で読み出す。
bool ReadCapturedImageAsLinearRGBA(const DirectX::ScratchImage& captured,
                                   std::vector<float>& outRgba,
                                   std::uint32_t& outWidth, std::uint32_t& outHeight);

} // namespace fbzz::renderer::detail
