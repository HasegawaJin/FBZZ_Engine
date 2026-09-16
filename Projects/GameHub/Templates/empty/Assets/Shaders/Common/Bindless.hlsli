// FBZZ Engine
// Bindless.hlsli | Common
// ResourceDescriptorHeap を添字で引くためのアクセサ。テーブル経路との縮退規則もここに置く
//
// WHY このヘッダーがあるか: ResourceDescriptorHeap[i] を直接書くと、非対応機への縮退と
//     «無効な添字» の扱いが各シェーダーへ散らばる。読み替えの規則を 1 か所へ集約する。
//
// 使い方:
//   Texture2D albedo = BindlessTexture2D(material.albedoIndex);
//   if (IsBindlessValid(material.albedoIndex)) { ... } else { 従来の t0 を読む }
//
// @see Docs/design/bindless.md
#ifndef FBZZ_COMMON_BINDLESS_HLSLI
#define FBZZ_COMMON_BINDLESS_HLSLI

#include "Platform/Backend.hlsli"

// ITexture::INVALID_BINDLESS_INDEX (C++ 側) と必ず同じ値にすること。
// WHY 0 を無効値にしないか: 0 はヒープ先頭の «有効な» ディスクリプタで、未設定と区別できない。
#define BINDLESS_INVALID_INDEX 0xFFFFFFFFu

// 添字が有効か。非対応機では C++ 側が常に INVALID を配るので、この判定だけで縮退が決まる。
bool IsBindlessValid(uint index)
{
    return index != BINDLESS_INVALID_INDEX;
}

#if BINDLESS_SUPPORTED

// WHY NonUniformResourceIndex で包むか: 同一 wave 内のレーンが別々の添字を持つ場合
//     (1 ドローに複数マテリアルが混ざる GPU 駆動経路)、包まないと «wave 内で添字は同一» と
//     見なされ、先頭レーンのリソースを全レーンが読む。単一マテリアルなら冗長だが、
//     ドライバが均一性を証明できる場合は最適化で消える。付け忘れる方の損が大きい。
Texture2D       BindlessTexture2D(uint index)   { return ResourceDescriptorHeap[NonUniformResourceIndex(index)]; }
Texture2DArray  BindlessTexture2DArray(uint index) { return ResourceDescriptorHeap[NonUniformResourceIndex(index)]; }
Texture3D       BindlessTexture3D(uint index)   { return ResourceDescriptorHeap[NonUniformResourceIndex(index)]; }
TextureCube     BindlessTextureCube(uint index) { return ResourceDescriptorHeap[NonUniformResourceIndex(index)]; }
ByteAddressBuffer BindlessByteBuffer(uint index) { return ResourceDescriptorHeap[NonUniformResourceIndex(index)]; }

#endif // BINDLESS_SUPPORTED

#endif // FBZZ_COMMON_BINDLESS_HLSLI
