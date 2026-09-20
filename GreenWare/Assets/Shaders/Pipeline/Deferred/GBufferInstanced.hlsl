// @file    GBufferInstanced.hlsl
// @brief   GBuffer.hlsl のインスタンシング変種。
// @author  Hasegawa Jin
// @date    2026-09-20
//
// @note 本体は GBuffer.hlsl と共有する。片方だけ直して «束ねたときだけ古い変換» になるのを
//       防ぐため、ここには入口の切り替えしか書かない。
// @see Docs/design/gpu-instancing.md
#define FBZZ_INSTANCED 1
#include "GBuffer.hlsl"
