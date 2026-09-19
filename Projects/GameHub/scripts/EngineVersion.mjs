/**
 * @file EngineVersion.mjs
 * @brief リポジトリ直下の CMakeLists.txt を Engine version の唯一の正本として読む。
 * @author Hasegawa Jin
 * @date 2026/09/17
 */

import { readFileSync } from 'node:fs';
import path from 'node:path';

/** `project(FBZZEngine VERSION x.y.z ...)` の x.y.z だけを取る。CMake/FBZZSDK.cmake の SDK ID と同じ規則。 */
const VERSION_PATTERN = /\bproject\s*\(\s*FBZZEngine\s+VERSION\s+([0-9]+(?:\.[0-9]+){2})\b/i;

/**
 * Engine version を返す。
 * @param {string} repositoryRoot CMakeLists.txt を持つリポジトリルート。
 * @returns {string} 例: "0.1.0"。読めなければ throw (推測で続行しない)。
 */
export function ReadEngineVersion(repositoryRoot) {
  const cmakeText = readFileSync(path.join(repositoryRoot, 'CMakeLists.txt'), 'utf8');
  const match = cmakeText.match(VERSION_PATTERN);
  if (!match) throw new Error('CMakeLists.txtからFBZZ Engine versionを取得できません。');
  return match[1];
}
