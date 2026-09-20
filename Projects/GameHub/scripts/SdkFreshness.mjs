/// @file    SdkFreshness.mjs
/// @brief   現在のソースに対応する SDK が公開済みかを検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-16

import { execFile } from 'node:child_process';
import { access, readFile, readdir, stat } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import { parse } from 'smol-toml';
import { ReadEngineVersion } from './EngineVersion.mjs';

const ExecuteFile = promisify(execFile);
const GIT_OUTPUT_LIMIT = 64 * 1024 * 1024;

/// @brief ファイルまたはディレクトリが存在するかを、検証処理を中断せずに確認する。
async function Exists(target) {
  try { await access(target); return true; } catch { return false; }
}

/// @brief 最新性判定に使う Git 出力を取得し、失敗時は最新性を推測せず停止する。
async function RunGit(repositoryRoot, args) {
  const { stdout } = await ExecuteFile('git', args, {
    cwd: repositoryRoot,
    encoding: 'utf8',
    maxBuffer: GIT_OUTPUT_LIMIT,
  });
  return stdout;
}

/// @note NUL 区切りを使い、非 ASCII パスの quote / escape による実体との不一致を避ける。
function ParseChangedPaths(statusOutput) {
  const entries = statusOutput.split('\0');
  const changedPaths = [];
  for (let index = 0; index < entries.length; index += 1) {
    const entry = entries[index];
    /// @note 各要素は「XY <path>」。末尾の空要素と壊れた行は無視する。
    if (entry.length < 4) continue;
    changedPaths.push(entry.slice(3));
    /// @note rename / copy だけは「新パス\0旧パス」の 2 要素で 1 件。旧パスは変更検出に使わない。
    if (entry[0] === 'R' || entry[0] === 'C') index += 1;
  }
  return changedPaths;
}

/// @return ファイルまたはディレクトリ配下で最も新しい更新時刻。存在しない場合は 0。
async function NewestModifiedTime(target) {
  let info;
  try {
    info = await stat(target);
  } catch {
    /// @note 削除済みファイルは git status に現れても stat できない。最新性の判定材料にはしない。
    return 0;
  }
  if (!info.isDirectory()) return info.mtimeMs;

  /// @note 未追跡ディレクトリは git status がフォルダー単位で 1 行にまとめるため、中身まで見る。
  let newest = info.mtimeMs;
  for (const entry of await readdir(target, { withFileTypes: true })) {
    newest = Math.max(newest, await NewestModifiedTime(path.join(target, entry.name)));
  }
  return newest;
}

/// @note SDK は version ごとに上書きするため、ID だけでは最新性を判定できない。変更ファイルも返す。
/// @see CMake/FBZZSDK.cmake
export async function ResolveExpectedSdkId(repositoryRoot) {
  const [revisionOutput, statusOutput] = await Promise.all([
    RunGit(repositoryRoot, ['rev-parse', '--short=12', 'HEAD']),
    RunGit(repositoryRoot, ['status', '--porcelain', '-z']),
  ]);

  return {
    sdkId: ReadEngineVersion(repositoryRoot),
    revision: revisionOutput.trim() || 'unknown',
    changedPaths: ParseChangedPaths(statusOutput),
  };
}

/// @brief GameHub と同じ構成の最新 SDK が完全に公開済みであることを保証する。
export async function AssertSdkIsCurrent(repositoryRoot, configuration) {
  const { sdkId, revision, changedPaths } = await ResolveExpectedSdkId(repositoryRoot);
  const sdkRoot = path.join(repositoryRoot, 'SDK', sdkId);
  const publishHint = `VS Codeで「CMake: Build SDK (${configuration})」を実行してください。`;

  if (!await Exists(sdkRoot)) {
    throw new Error(`SDKが最新ではありません。期待するSDK: ${sdkRoot}\n${publishHint}`);
  }

  const requiredPaths = [
    'fbzz-sdk.toml',
    'cmake/FBZZ/FBZZConfig.cmake',
    'cmake/FBZZ/FBZZConfigVersion.cmake',
    'cmake/FBZZ/FBZZTargets.cmake',
    'cmake/FBZZ/build.config.in',
    'share/fbzz/Assets/Shaders',
    'include/Engine/Scene/ScriptDllAbi.hpp',
    'include/Physics/World.hpp',
    'include/Math/Vector3.hpp',
    'include/Core/Logger.hpp',
    'include/Graphics/Renderer/RenderScene.hpp',
    `lib/${configuration}/FBZZCore.lib`,
    `lib/${configuration}/FBZZGraphics.lib`,
    `lib/${configuration}/FBZZMath.lib`,
    `lib/${configuration}/FBZZPhysics.lib`,
    `lib/${configuration}/FBZZEngine.lib`,
    `bin/${configuration}/FBZZMath.dll`,
    `bin/${configuration}/FBZZPhysics.dll`,
    `bin/${configuration}/FBZZEngine.dll`,
    `bin/${configuration}/FBZZCore.dll`,
    `bin/${configuration}/FBZZGraphics.dll`,
    `bin/${configuration}/published.stamp`,
    `tools/${configuration}/Editor/FBZZEditor.exe`,
    `tools/${configuration}/Editor/imgui.dll`,
    `tools/${configuration}/Editor/FBZZCore.dll`,
    `tools/${configuration}/Editor/FBZZGraphics.dll`,
  ];
  const availability = await Promise.all(requiredPaths.map(async (relativePath) => ({
    relativePath,
    exists: await Exists(path.join(sdkRoot, relativePath)),
  })));
  const missingPaths = availability.filter(({ exists }) => !exists).map(({ relativePath }) => relativePath);
  if (missingPaths.length > 0) {
    throw new Error(`SDK ${sdkId} の${configuration}構成が不完全です。\n不足: ${missingPaths.join(', ')}\n${publishHint}`);
  }

  const manifest = parse(await readFile(path.join(sdkRoot, 'fbzz-sdk.toml'), 'utf8'));
  if (manifest.sdk?.id !== sdkId) {
    throw new Error(`SDK manifestのIDが一致しません。期待: ${sdkId} / 実際: ${String(manifest.sdk?.id ?? '')}\n${publishHint}`);
  }

  /// @note SDK は同じ ID へ上書きする。公開時刻より新しいソースがあれば古い成果物と見なす。
  const publishedAt = (await stat(path.join(sdkRoot, 'bin', configuration, 'published.stamp'))).mtimeMs;
  const suspectPaths = new Set(changedPaths);

  const publishedRevision = String(manifest.sdk?.source_revision ?? '');
  if (publishedRevision !== revision) {
    /// @note commit だけなら実体は変わらない。pull 等で実体を書き換えた場合に mtime が公開時刻を超える。
    let changedSincePublish;
    try {
      changedSincePublish = await RunGit(repositoryRoot, ['diff', '--name-only', '-z', publishedRevision, 'HEAD']);
    } catch {
      throw new Error(`SDK ${sdkId} の公開元commit (${publishedRevision || '不明'}) を解決できません。\n${publishHint}`);
    }
    for (const relativePath of changedSincePublish.split('\0')) {
      if (relativePath) suspectPaths.add(relativePath);
    }
  }

  const staleSources = [];
  for (const relativePath of suspectPaths) {
    if (await NewestModifiedTime(path.resolve(repositoryRoot, relativePath)) > publishedAt) {
      staleSources.push(relativePath);
    }
  }
  if (staleSources.length > 0) {
    const preview = staleSources.slice(0, 5).join(', ');
    const suffix = staleSources.length > 5 ? ` ほか${staleSources.length - 5}件` : '';
    throw new Error(`SDK ${sdkId} の公開後にソースが変更されています。\n変更: ${preview}${suffix}\n${publishHint}`);
  }

  console.log(`SDK最新性チェック: OK (${sdkId}, ${configuration})`);
}
