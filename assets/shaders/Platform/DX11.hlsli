// FBZZ Engine
// DX11.hlsli | Platform
// SM 5.0 (DX11) プラットフォーム定義。Step 6b で DX12.hlsli に切り替える
#ifndef PLATFORM_DX11
#define PLATFORM_DX11

#define PLATFORM_DX11             1
#define WAVE_INTRINSICS_SUPPORTED 0  // SM 6.0+ 専用
#define RAY_QUERY_SUPPORTED       0  // SM 6.5+ 専用 (Inline Raytracing)

#endif // PLATFORM_DX11
