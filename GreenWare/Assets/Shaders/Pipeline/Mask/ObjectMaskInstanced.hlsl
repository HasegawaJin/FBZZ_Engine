// @file    ObjectMaskInstanced.hlsl
// @brief   ObjectMask.hlsl のインスタンシング変種。
// @author  Hasegawa Jin
// @date    2026-09-20
//
// @note 本体は ObjectMask.hlsl と共有する。片方だけ直して «束ねたときだけシルエットがずれる»
//       のを防ぐため、ここには入口の切り替えしか書かない。
// @see Docs/design/gpu-instancing.md
#define FBZZ_INSTANCED 1
#include "ObjectMask.hlsl"
