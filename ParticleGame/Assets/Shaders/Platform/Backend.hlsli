// FBZZ Engine
// Backend.hlsli | Platform
// アクティブなグラフィックスバックエンドのプラットフォーム定義を選択する間接ヘッダー。
//
// WHY: これまで各シェーダー (および ScriptCodeGen が生成するシェーダー) が
//      "Platform/DX11.hlsli" を直接 include しており、DX12 へ切り替える際に
//      全シェーダーを書き換える必要があった。バックエンド選択を本ファイルへ集約し、
//      シェーダー側は常に "Platform/Backend.hlsli" だけを include すれば済むようにする。
//
//      DX12を既定とし、互換用DX11シェーダーを作る場合だけFBZZ_BACKEND_DX11を定義する。
#ifndef FBZZ_PLATFORM_BACKEND_HLSLI
#define FBZZ_PLATFORM_BACKEND_HLSLI

#if defined(FBZZ_BACKEND_DX11)
    #include "Platform/DX11.hlsli"
#else
    #include "Platform/DX12.hlsli"
#endif

#endif // FBZZ_PLATFORM_BACKEND_HLSLI
