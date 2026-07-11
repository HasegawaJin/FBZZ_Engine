// FBZZ Engine
// Backend.hlsli | Platform
// アクティブなグラフィックスバックエンドのプラットフォーム定義を選択する間接ヘッダー。
//
// WHY: これまで各シェーダー (および ScriptCodeGen が生成するシェーダー) が
//      "Platform/DX11.hlsli" を直接 include しており、DX12 へ切り替える際に
//      全シェーダーを書き換える必要があった。バックエンド選択を本ファイルへ集約し、
//      シェーダー側は常に "Platform/Backend.hlsli" だけを include すれば済むようにする。
//
//      コンパイル時に FBZZ_BACKEND_DX12 を定義すると DX12.hlsli を選択する
//      (compile_shaders.bat / DXC 呼び出し側で /D FBZZ_BACKEND_DX12 を付与)。
//      未定義時は既存挙動どおり DX11 をデフォルトとする。
#ifndef FBZZ_PLATFORM_BACKEND_HLSLI
#define FBZZ_PLATFORM_BACKEND_HLSLI

#if defined(FBZZ_BACKEND_DX12)
    #include "Platform/DX12.hlsli"  // Step 6b で追加予定
#else
    #include "Platform/DX11.hlsli"
#endif

#endif // FBZZ_PLATFORM_BACKEND_HLSLI
