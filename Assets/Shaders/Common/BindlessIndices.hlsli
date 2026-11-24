/// @file    BindlessIndices.hlsli
/// @brief   ドローごとの bindless ディスクリプター添字。
/// @author  Hasegawa Jin
/// @date    2026-09-16
/// @note Graphics/Renderer/BindlessIndices.hpp と完全に一致させること。ずらすと別スロットの
/// @note 添字を読み、無関係なテクスチャが貼られる (コンパイルも実行も通るので見た目でしか気づけない)。
///
/// @note uint4 で束ねるか: 定数バッファは配列要素を 16 バイト境界へパディングするため、
/// @note uint gIndices[32] と書くと 512 バイト消費し、C++ 側の詰め方とも食い違う。
///
/// @see Docs/design/bindless.md
#ifndef FBZZ_COMMON_BINDLESS_INDICES_HLSLI
#define FBZZ_COMMON_BINDLESS_INDICES_HLSLI

#include "Common/Binding.hlsli"
#include "Common/Bindless.hlsli"

/// @note 旧ピクセル SRV テーブル (t0〜t31) と同じ幅。
#define BINDLESS_PIXEL_SLOT_COUNT  32
#define BINDLESS_VERTEX_SLOT_COUNT 4
#define BINDLESS_UAV_SLOT_COUNT    8

cbuffer BindlessIndicesConstants : register(CB_BINDLESS_INDICES)
{
    uint4 gBindlessPixel[BINDLESS_PIXEL_SLOT_COUNT / 4];
    uint4 gBindlessVertex[BINDLESS_VERTEX_SLOT_COUNT / 4];
    uint4 gBindlessUav[BINDLESS_UAV_SLOT_COUNT / 4];
};

/// @note 旧 t0〜t31 に対応する添字を引く。slot は Binding.hlsli の TEX_*_SLOT を渡す。
uint FbzzPixelSlot(uint slot)  { return gBindlessPixel[slot >> 2][slot & 3]; }
/// @note 旧 VS 側テーブルの添字。0 = instanceBuffer、1 = vsBuffers[0]、2 = vsBuffers[1]。
///
/// @note ピクセル側と分けるか: 旧モデルでは VS の t0 と PS の t0 が «別のテーブル» を指して
/// @note いた (ShaderVisibility で分岐)。bindless の添字空間は平坦なので、ここで分けないと
/// @note instanceBuffer と Albedo が同じ枠を奪い合う。
/// @note 具体的には SB_GPU_PARTICLES (t14) が、VS からは vsBuffers[0]、CS からは
/// @note srvBuffers[14] という別物を指す。VS 側の宣言には必ず FBZZ_VS_SBUFFER を使うこと。
uint FbzzVertexSlot(uint slot) { return gBindlessVertex[slot >> 2][slot & 3]; }
/// @note 旧 UAV テーブル (u0〜u7) の添字。
uint FbzzUavSlot(uint slot)    { return gBindlessUav[slot >> 2][slot & 3]; }

/// @note ---- 宣言マクロ ------------------------------------------------------------
/// @note 旧: Texture2D texAlbedo : register(TEX_ALBEDO);
/// @note 新: FBZZ_TEX2D(texAlbedo, TEX_ALBEDO_SLOT);
///
/// @note static 変数にするか: これにすると Sample 呼び出し側は 1 文字も変わらない。
/// @note 284 箇所ある参照を書き換えず、宣言 1 行の置き換えだけで移行できる。
#define FBZZ_TEX2D(name, slot) \
    static Texture2D name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_TEX2D_T(type, name, slot) \
    static Texture2D<type> name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_TEX3D(name, slot) \
    static Texture3D name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_TEX3D_T(type, name, slot) \
    static Texture3D<type> name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_TEXCUBE(name, slot) \
    static TextureCube name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_TEX2DARRAY(name, slot) \
    static Texture2DArray name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_TEX2DARRAY_T(type, name, slot) \
    static Texture2DArray<type> name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_TEXCUBE_T(type, name, slot) \
    static TextureCube<type> name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_SBUFFER_T(type, name, slot) \
    static StructuredBuffer<type> name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
#define FBZZ_BYTEBUFFER(name, slot) \
    static ByteAddressBuffer name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(slot))]
/// @note 頂点シェーダーが読む StructuredBuffer (instanceBuffer / vsBuffers)。
/// @note 添字空間が違うので、VS から読むものに FBZZ_SBUFFER_T を使ってはいけない。
#define FBZZ_VS_SBUFFER(type, name, slot) \
    static StructuredBuffer<type> name = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzVertexSlot(slot))]

/// @note ---- UAV (u0〜u7) ----------------------------------------------------------
/// @note NonUniformResourceIndex を付けないか: UAV は Dispatch 単位で 1 つに固定されており、
/// @note wave 内で添字が割れることがない。SRV 側と違って包む理由がない。
#define FBZZ_RWTEX2D(name, slot) \
    static RWTexture2D<float4> name = ResourceDescriptorHeap[FbzzUavSlot(slot)]
#define FBZZ_RWTEX2D_T(type, name, slot) \
    static RWTexture2D<type> name = ResourceDescriptorHeap[FbzzUavSlot(slot)]
#define FBZZ_RWTEX3D(name, slot) \
    static RWTexture3D<float4> name = ResourceDescriptorHeap[FbzzUavSlot(slot)]
#define FBZZ_RWTEX3D_T(type, name, slot) \
    static RWTexture3D<type> name = ResourceDescriptorHeap[FbzzUavSlot(slot)]
#define FBZZ_RWSBUFFER_T(type, name, slot) \
    static RWStructuredBuffer<type> name = ResourceDescriptorHeap[FbzzUavSlot(slot)]
#define FBZZ_RWBYTEBUFFER(name, slot) \
    static RWByteAddressBuffer name = ResourceDescriptorHeap[FbzzUavSlot(slot)]
#define FBZZ_RWTEX2DARRAY(name, slot) \
    static RWTexture2DArray<float4> name = ResourceDescriptorHeap[FbzzUavSlot(slot)]
#define FBZZ_RWTEX2DARRAY_T(type, name, slot) \
    static RWTexture2DArray<type> name = ResourceDescriptorHeap[FbzzUavSlot(slot)]

#endif
