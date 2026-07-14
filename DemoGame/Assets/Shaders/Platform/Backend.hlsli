// FBZZ Engine
// Backend.hlsli | Platform
// DX12を既定とするシェーダーバックエンド選択
#ifndef FBZZ_PLATFORM_BACKEND_HLSLI
#define FBZZ_PLATFORM_BACKEND_HLSLI

#if defined(FBZZ_BACKEND_DX11)
    #include "Platform/DX11.hlsli"
#else
    #include "Platform/DX12.hlsli"
#endif

#endif // FBZZ_PLATFORM_BACKEND_HLSLI
