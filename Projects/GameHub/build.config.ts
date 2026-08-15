// FBZZ Engine
// build.config.ts | GameHub
// GameHubのビルド構成とVite出力方針を一元管理する

import type { BuildOptions } from 'vite';

const GAME_HUB_BUILD_CONFIGURATIONS = ['Debug', 'Development', 'Release'] as const;

export type GameHubBuildConfiguration = typeof GAME_HUB_BUILD_CONFIGURATIONS[number];

/** 環境変数を検証し、未指定または不正値の場合は日常開発向けのDevelopmentへフォールバックする。 */
function ResolveBuildConfiguration(): GameHubBuildConfiguration {
  const configured = process.env.FBZZ_BUILD_CONFIG;
  return GAME_HUB_BUILD_CONFIGURATIONS.includes(configured as GameHubBuildConfiguration)
    ? configured as GameHubBuildConfiguration
    : 'Development';
}

export const GAME_HUB_BUILD_CONFIGURATION = ResolveBuildConfiguration();

/** Debug・Development・Releaseごとのデバッグ情報と圧縮方針をViteへ提供する。 */
export function CreateViteBuildOptions(): BuildOptions {
  if (GAME_HUB_BUILD_CONFIGURATION === 'Debug') {
    return { minify: false, sourcemap: 'inline' };
  }
  if (GAME_HUB_BUILD_CONFIGURATION === 'Development') {
    return { minify: false, sourcemap: true };
  }
  return { minify: 'esbuild', sourcemap: false };
}
