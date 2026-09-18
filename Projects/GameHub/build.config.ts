/**
 * @file build.config.ts
 * @brief GameHub のビルド構成・Vite 出力方針・ソースへ注入する定数を一元管理する。
 * @author Hasegawa Jin
 * @date 2026/07/20
 */

import path from 'node:path';
import type { BuildOptions } from 'vite';
import { ReadEngineVersion } from './scripts/EngineVersion.mjs';

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

/**
 * Engine version。正本はリポジトリ直下の CMakeLists.txt で、GameHub 側に手書きの複製を置かない。
 * WHY: SdkFreshness.mjs (SDK ID) と contracts.ts (互換判定) が別々の値を持つと、版を上げた直後に
 *      全プロジェクトへ「Engine 向けバージョン不一致」の警告が出る。
 */
export const ENGINE_VERSION = ReadEngineVersion(path.resolve(__dirname, '..', '..'));

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

/** ソースへ埋め込む定数。contracts.ts の ENGINE_VERSION はここから注入される。 */
export function CreateViteDefine(): Record<string, string> {
  return { __FBZZ_ENGINE_VERSION__: JSON.stringify(ENGINE_VERSION) };
}
