// @file    GBufferSkinned.hlsl
// @brief   GBuffer.hlsl のスキンド変種。b7 のパレットで VS が変形する。
// @author  Hasegawa Jin
// @date    2026-09-20
//
// @note コンピュートスキニングが効いているフレームはこれを使わない。変形済みの頂点が静的メッシュと
//       同じレイアウトで来るので、素の GBuffer.hlsl でそのまま描ける。これはその経路が無いときの
//       フォールバック。
// @note 本体 (PS) は GBuffer.hlsl と共有する。片方だけ直して «スキンドだけ材質の読み方が古い» に
//       なるのを防ぐため、ここには入口の切り替えしか書かない。
// @see Docs/design/pipeline-boundary.md §3
#define FBZZ_SKINNED 1
#include "GBuffer.hlsl"
