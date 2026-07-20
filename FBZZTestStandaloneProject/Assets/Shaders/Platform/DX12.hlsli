// FBZZ Engine
// DX12.hlsli | Platform
// DirectX 12 シェーダーバックエンドの機能フラグ
#ifndef FBZZ_PLATFORM_DX12_HLSLI
#define FBZZ_PLATFORM_DX12_HLSLI

#ifndef PLATFORM_DX12
#define PLATFORM_DX12             1
#endif

// WHAT: DX12 は DXC / SM 6.8 を標準とし、Wave 命令を利用可能にする。
// WHY: 実行時の個別機能対応は D3D12 feature query で判定し、非対応 GPU では
//      該当パス自体を選ばない。これはコンパイル対象が命令を記述できることを表す。
#ifndef WAVE_INTRINSICS_SUPPORTED
#define WAVE_INTRINSICS_SUPPORTED 1
#endif

#ifndef RAY_QUERY_SUPPORTED
#define RAY_QUERY_SUPPORTED       1
#endif

#endif // FBZZ_PLATFORM_DX12_HLSLI
