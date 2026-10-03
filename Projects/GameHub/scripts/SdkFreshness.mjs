/// @file    SdkFreshness.mjs
/// @brief   現在のソースに対応する SDK が公開済みかを検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-16

import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { access, readFile, readdir, stat } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import { parse } from 'smol-toml';
import { ReadEngineVersion } from './EngineVersion.mjs';

const ExecuteFile = promisify(execFile);
const GIT_OUTPUT_LIMIT = 64 * 1024 * 1024;
const SDK_EDITOR_LIBRARIES = ['Math', 'Physics', 'Fluid', 'Core', 'Graphics', 'Engine'];
const SDK_EDITOR_DLLS = [...SDK_EDITOR_LIBRARIES.map((library) => `FBZZ${library}.dll`), 'imgui.dll'];

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

/// @brief 公開状態と必須配置を確認し、配布時には記録された SHA256 も照合する。
export async function AssertSdkContract(sdkRoot, configuration, { verifyHashes = false } = {}) {
  if (!['Debug', 'Development', 'Release'].includes(configuration)) {
    throw new Error(`未対応のSDK構成です: ${configuration}`);
  }
  const manifest = parse(await readFile(path.join(sdkRoot, 'fbzz-sdk.toml'), 'utf8'));
  const runtime = manifest.runtime;
  const record = manifest.configurations?.[configuration];
  if (manifest.sdk?.schema !== 2 || manifest.sdk.architecture !== 'x64'
      || !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(manifest.sdk.id ?? '')
      || typeof runtime?.dx12_enabled !== 'boolean'
      || !/^[0-9a-f]{64}$/.test(runtime.fingerprint ?? '')) {
    throw new Error('SDK manifestのschemaまたはランタイム契約が未対応です。SDKを再公開してください。');
  }
  if (record?.validated !== true || record.fingerprint !== runtime.fingerprint) {
    throw new Error(`SDKの${configuration}構成は現在のランタイム契約で公開・検証されていません。`);
  }
  const requiredPaths = [
    'cmake/FBZZ/FBZZConfig.cmake', 'cmake/FBZZ/FBZZConfigVersion.cmake',
    'cmake/FBZZ/FBZZTargets.cmake', 'cmake/FBZZ/build.config.in',
    'cmake/FBZZ/FBZZAgilitySDK.cmake', 'cmake/FBZZ/AgilitySDKExports.cpp.in', 'cmake/FBZZ/StageAgilitySDK.cmake',
    'include/Engine/Scene/ScriptDllAbi.hpp', 'include/Graphics/Renderer/RenderScene.hpp',
    'include/Physics/World.hpp', 'include/Fluid/FluidSolver.hpp', 'include/Math/Vector3.hpp', 'include/Core/Logger.hpp',
    'share/fbzz/Assets/Shaders', `bin/${configuration}/published.stamp`,
    `lib/${configuration}/imgui.lib`, `bin/${configuration}/imgui.dll`,
    `tools/${configuration}/Editor/FBZZEditor.exe`, `tools/${configuration}/Editor/imgui.dll`,
  ];
  for (const library of SDK_EDITOR_LIBRARIES) {
    requiredPaths.push(`lib/${configuration}/FBZZ${library}.lib`, `bin/${configuration}/FBZZ${library}.dll`,
      `tools/${configuration}/Editor/FBZZ${library}.dll`);
  }
  const editorDlls = [...SDK_EDITOR_DLLS, configuration === 'Debug' ? 'assimp-vc145-mtd.dll' : 'assimp-vc145-mt.dll'];
  for (const name of editorDlls) requiredPaths.push(`bin/${configuration}/${name}`, `tools/${configuration}/Editor/${name}`);
  if (runtime.dx12_enabled) {
    if (!Number.isInteger(runtime.agility_sdk_version) || runtime.agility_sdk_version <= 0
        || runtime.agility_path !== '.\\D3D12\\' || typeof runtime.core_file_version !== 'string'
        || !runtime.core_file_version || !runtime.agility_package || !runtime.dxc_version) {
      throw new Error('SDKのAgility/DXC版・配置契約が不完全です。');
    }
    for (const field of ['core', 'layers', 'dxc_exe', 'dxc_compiler', 'dxc_validator']) {
      if (!/^[0-9a-f]{64}$/.test(runtime[`${field}_sha256`] ?? '')) {
        throw new Error(`SDK manifestの${field}_sha256が不正です。`);
      }
    }
    for (const prefix of [`bin/${configuration}`, `tools/${configuration}/Editor`]) {
      requiredPaths.push(`${prefix}/D3D12/D3D12Core.dll`, `${prefix}/dxcompiler.dll`, `${prefix}/dxil.dll`, `${prefix}/WinPixEventRuntime.dll`);
      if (configuration !== 'Release') requiredPaths.push(`${prefix}/D3D12/d3d12SDKLayers.dll`);
      else if (await Exists(path.join(sdkRoot, prefix, 'D3D12/d3d12SDKLayers.dll'))) {
        throw new Error(`SDK Releaseに開発レイヤーが残っています: ${prefix}/D3D12/d3d12SDKLayers.dll`);
      }
    }
    for (const name of ['dxc.exe', 'dxcompiler.dll', 'dxil.dll']) requiredPaths.push(`tools/${configuration}/DXC/${name}`);
    for (const name of ['LICENSE', 'LICENSE.txt', 'LICENSE-CODE.txt', 'VERSION', 'distributable files.txt']) requiredPaths.push(`share/fbzz/licenses/AgilitySDK/${name}`);
    for (const name of ['LICENSE', 'LICENCE-MIT.txt', 'LICENSE-LLVM.txt', 'LICENSE-MS.txt', 'VERSION']) requiredPaths.push(`share/fbzz/licenses/DXC/${name}`);
    for (const name of ['LICENSE', 'VERSION', 'ThirdPartyNotices.txt']) requiredPaths.push(`share/fbzz/licenses/WinPixEventRuntime/${name}`);
    for (const relativePath of [...requiredPaths]) {
      if (relativePath.startsWith('share/fbzz/licenses/')) {
        requiredPaths.push(`tools/${configuration}/Editor/EngineLicenses/${relativePath.slice('share/fbzz/licenses/'.length)}`);
      }
    }
  }
  const missing = [];
  const files = manifest.files?.[configuration];
  for (const relativePath of requiredPaths) {
    const absolutePath = path.join(sdkRoot, relativePath);
    if (!await Exists(absolutePath)) missing.push(relativePath);
    else {
      const info = await stat(absolutePath);
      if (relativePath === 'share/fbzz/Assets/Shaders') {
        if (!info.isDirectory()) throw new Error(`SDKの必須ディレクトリが不正です: ${relativePath}`);
      } else if (!info.isFile() || !/^[0-9a-f]{64}$/.test(files?.[relativePath] ?? '')) {
        throw new Error(`SDKの必須ファイルまたは公開ハッシュが不正です: ${relativePath}`);
      }
    }
  }
  if (missing.length) throw new Error(`SDK ${configuration}が不完全です。不足: ${missing.join(', ')}`);
  for (const name of editorDlls) {
    if (files[`bin/${configuration}/${name}`] !== files[`tools/${configuration}/Editor/${name}`]) {
      throw new Error(`SDK Editorと公開SDKのDLLが一致しません: ${name}`);
    }
  }
  if (runtime.dx12_enabled) {
    for (const prefix of [`bin/${configuration}`, `tools/${configuration}/Editor`]) {
      for (const [relativeFile, field] of [['D3D12/D3D12Core.dll', 'core'], ['dxcompiler.dll', 'dxc_compiler'], ['dxil.dll', 'dxc_validator']]) {
        if (files[`${prefix}/${relativeFile}`] !== runtime[`${field}_sha256`]) {
          throw new Error(`SDKの公開構成がランタイム版と一致しません: ${prefix}/${relativeFile}`);
        }
      }
      if (configuration !== 'Release' && files[`${prefix}/D3D12/d3d12SDKLayers.dll`] !== runtime.layers_sha256) {
        throw new Error(`SDKのLayers版がランタイム契約と一致しません: ${prefix}`);
      }
    }
    for (const [name, field] of [['dxc.exe', 'dxc_exe'], ['dxcompiler.dll', 'dxc_compiler'], ['dxil.dll', 'dxc_validator']]) {
      if (files[`tools/${configuration}/DXC/${name}`] !== runtime[`${field}_sha256`]) {
        throw new Error(`SDKのDXCツール版がランタイム契約と一致しません: ${name}`);
      }
    }
    const agilityVersion = await readFile(path.join(sdkRoot, 'share/fbzz/licenses/AgilitySDK/VERSION'), 'utf8');
    const dxcVersion = await readFile(path.join(sdkRoot, 'share/fbzz/licenses/DXC/VERSION'), 'utf8');
    for (const [source, name, value] of [
      [agilityVersion, 'FBZZ_AGILITY_PACKAGE_VERSION', runtime.agility_package],
      [agilityVersion, 'FBZZ_AGILITY_CORE_FILE_VERSION', runtime.core_file_version],
      [agilityVersion, 'FBZZ_AGILITY_CORE_SHA256', runtime.core_sha256],
      [agilityVersion, 'FBZZ_AGILITY_LAYERS_SHA256', runtime.layers_sha256],
      [dxcVersion, 'FBZZ_DXC_PACKAGE_VERSION', runtime.dxc_version],
      [dxcVersion, 'FBZZ_DXC_EXECUTABLE_SHA256', runtime.dxc_exe_sha256],
      [dxcVersion, 'FBZZ_DXC_COMPILER_SHA256', runtime.dxc_compiler_sha256],
      [dxcVersion, 'FBZZ_DXC_VALIDATOR_SHA256', runtime.dxc_validator_sha256],
    ]) {
      if (!source.includes(`set(${name} "${value}")`)) throw new Error(`SDKのVERSIONとmanifestが一致しません: ${name}`);
    }
    if (!agilityVersion.includes(`set(FBZZ_AGILITY_SDK_VERSION ${runtime.agility_sdk_version})`)) {
      throw new Error('SDKのAgility整数版がVERSIONと一致しません。');
    }
  }
  if (verifyHashes) {
    const normalizedRoot = `${path.resolve(sdkRoot)}${path.sep}`;
    for (const [relativePath, expectedHash] of Object.entries(files ?? {})) {
      const filePath = path.resolve(sdkRoot, relativePath);
      if (!filePath.startsWith(normalizedRoot) || !/^[0-9a-f]{64}$/.test(expectedHash)) {
        throw new Error(`SDKの公開ファイル記録が不正です: ${relativePath}`);
      }
      const actualHash = createHash('sha256').update(await readFile(filePath)).digest('hex');
      if (actualHash !== expectedHash) throw new Error(`SDKファイルのSHA256が一致しません: ${relativePath}`);
    }
  }
  return manifest;
}

/// @brief GameHub と同じ構成の最新 SDK が完全に公開済みであることを保証する。
export async function AssertSdkIsCurrent(repositoryRoot, configuration) {
  const { sdkId, revision, changedPaths } = await ResolveExpectedSdkId(repositoryRoot);
  const sdkRoot = path.join(repositoryRoot, 'SDK', sdkId);
  const publishHint = `VS Codeで「CMake: Build SDK (${configuration})」を実行してください。`;

  if (!await Exists(sdkRoot)) {
    throw new Error(`SDKが最新ではありません。期待するSDK: ${sdkRoot}\n${publishHint}`);
  }

  let manifest;
  try {
    manifest = await AssertSdkContract(sdkRoot, configuration);
  } catch (error) {
    throw new Error(`${error.message}\n${publishHint}`);
  }
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
