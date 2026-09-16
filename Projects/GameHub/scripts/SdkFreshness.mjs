// FBZZ Engine
// SdkFreshness.mjs | GameHub
// 現在のソースに対応するSDKが公開済みかを検証する

import { execFile } from 'node:child_process';
import { access, readFile, readdir, stat } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import { parse } from 'smol-toml';

const ExecuteFile = promisify(execFile);
const GIT_OUTPUT_LIMIT = 64 * 1024 * 1024;

/** ファイルまたはディレクトリが存在するかを、検証処理を中断せずに確認する。 */
async function Exists(target) {
  try { await access(target); return true; } catch { return false; }
}

/** 最新性判定に使うGit出力を取得し、失敗時は最新性を推測せず停止する。 */
async function RunGit(repositoryRoot, args) {
  const { stdout } = await ExecuteFile('git', args, {
    cwd: repositoryRoot,
    encoding: 'utf8',
    maxBuffer: GIT_OUTPUT_LIMIT,
  });
  return stdout;
}

/**
 * `git status --porcelain -z` から、worktreeで変更された相対パスを取り出す。
 * NUL区切りを使うのは、非ASCIIパスがquote/escapeされて実体と一致しなくなるのを避けるため。
 */
function ParseChangedPaths(statusOutput) {
  const entries = statusOutput.split('\0');
  const changedPaths = [];
  for (let index = 0; index < entries.length; index += 1) {
    const entry = entries[index];
    // 各要素は「XY <path>」。末尾の空要素と壊れた行は無視する。
    if (entry.length < 4) continue;
    changedPaths.push(entry.slice(3));
    // rename/copyだけは「新パス\0旧パス」の2要素で1件。旧パスは変更検出に使わない。
    if (entry[0] === 'R' || entry[0] === 'C') index += 1;
  }
  return changedPaths;
}

/** ファイルまたはディレクトリ配下で最も新しい更新時刻を返す。存在しない場合は0。 */
async function NewestModifiedTime(target) {
  let info;
  try {
    info = await stat(target);
  } catch {
    // 削除済みファイルはgit statusに現れてもstatできない。最新性の判定材料にはしない。
    return 0;
  }
  if (!info.isDirectory()) return info.mtimeMs;

  // 未追跡ディレクトリはgit statusがフォルダー単位で1行にまとめるため、中身まで見る。
  let newest = info.mtimeMs;
  for (const entry of await readdir(target, { withFileTypes: true })) {
    newest = Math.max(newest, await NewestModifiedTime(path.join(target, entry.name)));
  }
  return newest;
}

/**
 * CMake/FBZZSDK.cmakeと同じ規則で、現在のソースに対応するSDK IDを解決する。
 * SDKはversionごとに1つで上書き更新されるため、IDはEngine versionそのもの。
 * 「公開後に編集されていないか」はIDでは判定できないので、変更ファイル一覧も返す。
 */
export async function ResolveExpectedSdkId(repositoryRoot) {
  const [cmakeText, revisionOutput, statusOutput] = await Promise.all([
    readFile(path.join(repositoryRoot, 'CMakeLists.txt'), 'utf8'),
    RunGit(repositoryRoot, ['rev-parse', '--short=12', 'HEAD']),
    RunGit(repositoryRoot, ['status', '--porcelain', '-z']),
  ]);
  const versionMatch = cmakeText.match(/\bproject\s*\(\s*FBZZEngine\s+VERSION\s+([0-9]+(?:\.[0-9]+){2})\b/i);
  if (!versionMatch) throw new Error('CMakeLists.txtからFBZZ Engine versionを取得できません。');

  return {
    sdkId: versionMatch[1],
    revision: revisionOutput.trim() || 'unknown',
    changedPaths: ParseChangedPaths(statusOutput),
  };
}

/** GameHubと同じ構成の最新SDKが完全に公開済みであることを保証する。 */
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
    `lib/${configuration}/FBZZMath.lib`,
    `lib/${configuration}/FBZZPhysics.lib`,
    `lib/${configuration}/FBZZEngine.lib`,
    `bin/${configuration}/FBZZMath.dll`,
    `bin/${configuration}/FBZZPhysics.dll`,
    `bin/${configuration}/FBZZEngine.dll`,
    `bin/${configuration}/published.stamp`,
    `tools/${configuration}/Editor/FBZZEditor.exe`,
    `tools/${configuration}/Editor/imgui.dll`,
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

  // SDKは常に同じIDへ上書きされるため、IDの一致は「最新」を意味しない。
  // 公開時刻より新しいソースが1つでもあれば古い成果物と見なす。
  const publishedAt = (await stat(path.join(sdkRoot, 'bin', configuration, 'published.stamp'))).mtimeMs;
  const suspectPaths = new Set(changedPaths);

  const publishedRevision = String(manifest.sdk?.source_revision ?? '');
  if (publishedRevision !== revision) {
    // 公開時のcommitからHEADまでに触れたファイルも、mtime比較の対象へ加える。
    // WHY: commitしただけならファイル実体は書き換わらず、SDKは古くならない。pullや
    //      checkoutで中身が書き換わった場合だけmtimeが公開時刻を追い越し、staleになる。
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
