/// @file build.config.ts
/// @brief GameHub のビルド構成・Vite 出力方針・ソースへ注入する定数を一元管理する。
/// @author Hasegawa Jin
/// @date 2026/07/20

import path from 'node:path';
import type { BuildOptions } from 'vite';
import { ReadEngineVersion } from './scripts/EngineVersion.mjs';

const GAME_HUB_BUILD_CONFIGURATIONS = ['Debug', 'Development', 'Release'] as const;

export type GameHubBuildConfiguration = typeof GAME_HUB_BUILD_CONFIGURATIONS[number];

/// @note 未指定の開発サーバーは Development として扱う。
function ResolveBuildConfiguration(): GameHubBuildConfiguration {
  const configured = process.env.FBZZ_BUILD_CONFIG;
  return GAME_HUB_BUILD_CONFIGURATIONS.includes(configured as GameHubBuildConfiguration)
    ? configured as GameHubBuildConfiguration
    : 'Development';
}

export const GAME_HUB_BUILD_CONFIGURATION = ResolveBuildConfiguration();

/// @note Engine の版は CMake と SDK の正本を共有する。
export const ENGINE_VERSION = ReadEngineVersion(path.resolve(__dirname, '..', '..'));

export function CreateViteBuildOptions(): BuildOptions {
  if (GAME_HUB_BUILD_CONFIGURATION === 'Debug') {
    return { minify: false, sourcemap: 'inline' };
  }
  if (GAME_HUB_BUILD_CONFIGURATION === 'Development') {
    return { minify: false, sourcemap: true };
  }
  return { minify: 'esbuild', sourcemap: false };
}

/// @note 配布構成は初回の SDK 選択にも使い、既存の有効なユーザー設定は保持する。
export function CreateViteDefine(): Record<string, string> {
  return {
    __FBZZ_ENGINE_VERSION__: JSON.stringify(ENGINE_VERSION),
    __FBZZ_BUILD_CONFIGURATION__: JSON.stringify(GAME_HUB_BUILD_CONFIGURATION),
  };
}
