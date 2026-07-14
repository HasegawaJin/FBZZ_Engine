// FBZZ Engine
// DX12.hlsli | Platform
// DirectX 12 シェーダーバックエンドの機能フラグ
#ifndef FBZZ_PLATFORM_DX12_HLSLI
#define FBZZ_PLATFORM_DX12_HLSLI

#ifndef PLATFORM_DX12
#define PLATFORM_DX12             1
#endif

// WHAT: 初期移行では既存 SM 5.0 DXBC を流用するため Wave 命令を無効に保つ。
#ifndef WAVE_INTRINSICS_SUPPORTED
#define WAVE_INTRINSICS_SUPPORTED 0
#endif

#ifndef RAY_QUERY_SUPPORTED
#define RAY_QUERY_SUPPORTED       0
#endif

#endif // FBZZ_PLATFORM_DX12_HLSLI
