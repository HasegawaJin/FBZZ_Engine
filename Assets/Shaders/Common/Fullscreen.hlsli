/// @file    Fullscreen.hlsli
/// @brief   頂点バッファなしの全画面三角形を共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-14

/// @note Fullscreen.hlsli | Common
/// @note 全画面三角形の頂点生成を全 PostProcess シェーダーで共有する
#ifndef FBZZ_FULLSCREEN_HLSLI
#define FBZZ_FULLSCREEN_HLSLI

/// @note 全画面パスごとに同じ UV/クリップ座標計算を複製すると、
/// @note 座標系の修正が一部のパスだけに反映される。頂点バッファを使わない契約は維持し、
/// @note 共有型と関数だけを提供して各パスの PS ロジックは自由に残す。
struct FBZZFullscreenVertex
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FBZZFullscreenVertex FBZZMakeFullscreenVertex(uint vertexId)
{
    FBZZFullscreenVertex vertex;
    vertex.uv = float2((vertexId & 1u) != 0u ? 2.0f : 0.0f,
                       (vertexId & 2u) != 0u ? 2.0f : 0.0f);
    /// @note DX のテクスチャ UV と NDC の Y 軸差をここで一度だけ吸収する。
    vertex.svPosition = float4(vertex.uv * float2(2.0f, -2.0f)
                             + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vertex;
}

#endif ///< @note FBZZ_FULLSCREEN_HLSLI
