/// @file FroxelIntegrate.cs.hlsl
/// @brief フロクセルを手前から奥へ積分して透過率込みの霧を作る (フロクセル霧のパス 2)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// 1 スレッド = 1 本の Z 列。手前のスライスから順に「そこまでの透過率」を掛けながら
// 散乱を足していき、各スライスに「カメラからそこまでの積算」を書き戻す。
// 出力の rgb が加算する光、a が背景に掛ける透過率。
//
// WHY 列ごとの直列ループにするか: 積分はスライス間で完全に依存するので並列化できない。
//     グリッドが 160x90 なら 14400 スレッドが同時に走り、1 スレッドあたりの
//     ループは 64 回。GPU の占有率としてはこれで十分に埋まる。

#include "Common/FroxelFogConstants.hlsli"
#include "Platform/Backend.hlsli"

Texture3D<float4>   gScatter    : register(TEX_FROXEL_SCATTER);
RWTexture3D<float4> gIntegrated : register(UAV_FROXEL_INTEGRATED);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= froxelGridX || id.y >= froxelGridY)
        return;

    float3 accumulated  = float3(0.0f, 0.0f, 0.0f);
    float  transmittance = 1.0f;
    float  prevViewZ     = froxelNear;

    for (uint slice = 0u; slice < froxelGridZ; ++slice) {
        const uint3 coord = uint3(id.xy, slice);
        const float4 sample = gScatter.Load(int4(coord, 0));

        const float viewZ     = FBZZ_FroxelSliceToViewZ(float(slice) + 1.0f);
        const float thickness = max(viewZ - prevViewZ, 0.0f);
        prevViewZ = viewZ;

        const float extinction = sample.a;
        const float sliceTrans = exp(-extinction * thickness);

        // スライス内での散乱の解析積分。
        //   ∫0..d S * exp(-σt) dt = S * (1 - exp(-σd)) / σ
        // WHY 素朴に S*d としないか: 濃い霧ではスライス内で光がほぼ吸われきるため、
        //     厚みに比例させると密度を上げるほど明るくなるという逆転が起きる。
        const float3 sliceScatter = (extinction > 1e-5f)
            ? sample.rgb * (1.0f - sliceTrans) / extinction
            : sample.rgb * thickness;

        accumulated  += sliceScatter * transmittance;
        transmittance *= sliceTrans;

        gIntegrated[coord] = float4(accumulated, transmittance);
    }
}
