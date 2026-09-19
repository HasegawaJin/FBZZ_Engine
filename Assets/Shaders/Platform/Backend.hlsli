// FBZZ Engine
// Backend.hlsli | Platform
// バックエンド機能フラグの唯一の入口。シェーダーはこれだけを include する
//
// WHY このファイルがあるか: 以前は各シェーダー (および ScriptCodeGen の生成物) が
//     "Platform/DX11.hlsli" を直接 include していたため、バックエンドを変えると
//     全シェーダーを書き換える必要があった。選択をここへ集約してある。
// NOTE: DirectX 11 サポートは v1.0 で終了した (Docs/design/dx11-removal.md)。
//       DX12 / SM 6.x が唯一のターゲットだが、次のバックエンドを足すときに
//       再びここだけで済むよう、この間接層は意図的に残している。
#ifndef FBZZ_PLATFORM_BACKEND_HLSLI
#define FBZZ_PLATFORM_BACKEND_HLSLI

#include "Platform/DX12.hlsli"

#endif // FBZZ_PLATFORM_BACKEND_HLSLI
