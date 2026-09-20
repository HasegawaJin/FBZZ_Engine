// @file    ShadowMapInstanced.hlsl
// @brief   ShadowMap.hlsl のインスタンシング変種。
// @author  Hasegawa Jin
// @date    2026-09-20
//
// @note 本体は ShadowMap.hlsl と共有する。片方だけ直して «影だけ古い変換» になるのを防ぐため、
//       ここには入口の切り替えしか書かない。
// @see Docs/design/gpu-instancing.md
#define FBZZ_INSTANCED 1
#include "ShadowMap.hlsl"
