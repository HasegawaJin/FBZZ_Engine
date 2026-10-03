/// @file    configStore.ts
/// @brief   hub_config.toml を読み書きする永続化層と SDK の自動検出。
/// @author  Hasegawa Jin
/// @date    2026-07-19

import { app } from 'electron';
import { createReadStream } from 'node:fs';
import { createHash } from 'node:crypto';
import { access, mkdir, readFile, readdir, stat, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { parse, stringify } from 'smol-toml';
import {
  ENGINE_VERSION,
  GAME_HUB_BUILD_CONFIGURATION,
  type HubSettings,
  type HubTheme,
  type SdkBuildConfiguration,
} from '../shared/contracts';
import { EMPTY_UPDATE_STATE, type UpdateState } from './updateChecker';

export interface ConfigProject {
  path: string;
  lastOpened: string;
}

interface HubConfig {
  settings: HubSettings;
  projects: ConfigProject[];
  update: UpdateState;
}

const DEFAULT_CONFIG: HubConfig = {
  settings: {
    editorExe: '',
    sdkRoot: '',
    sdkId: '',
    sdkConfiguration: GAME_HUB_BUILD_CONFIGURATION,
    theme: 'modern',
    checkForUpdates: true,
  },
  projects: [],
  update: { ...EMPTY_UPDATE_STATE },
};

const SDK_EDITOR_LIBRARIES = ['Math', 'Physics', 'Fluid', 'Core', 'Graphics', 'Engine'] as const;
const SDK_EDITOR_DLLS = [...SDK_EDITOR_LIBRARIES.map((library) => `FBZZ${library}.dll`), 'imgui.dll'];

function GetSdkEditorDlls(configuration: SdkBuildConfiguration): string[] {
  return [...SDK_EDITOR_DLLS, configuration === 'Debug' ? 'assimp-vc145-mtd.dll' : 'assimp-vc145-mt.dll'];
}

function asString(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function normalizeProjectPath(projectPath: string): string {
  return path.normalize(path.resolve(projectPath));
}

async function exists(target: string): Promise<boolean> {
  try {
    await access(target);
    return true;
  } catch {
    return false;
  }
}

async function isSdkRoot(candidate: string, configuration: SdkBuildConfiguration = 'Development'): Promise<boolean> {
  if (!candidate) return false;
  try {
    const document = parse(await readFile(path.join(candidate, 'fbzz-sdk.toml'), 'utf8')) as Record<string, unknown>;
    const sdk = document.sdk as Record<string, unknown> | undefined;
    const runtime = document.runtime as Record<string, unknown> | undefined;
    const configurations = document.configurations as Record<string, Record<string, unknown>> | undefined;
    const record = configurations?.[configuration];
    const files = (document.files as Record<string, Record<string, unknown>> | undefined)?.[configuration];
    if (sdk?.schema !== 2 || sdk.architecture !== 'x64'
        || !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(asString(sdk.id))
        || typeof runtime?.dx12_enabled !== 'boolean'
        || !/^[0-9a-f]{64}$/.test(asString(runtime.fingerprint))
        || record?.validated !== true || record.fingerprint !== runtime.fingerprint) return false;
    const requiredPaths = [
      'cmake/FBZZ/FBZZConfig.cmake', 'cmake/FBZZ/FBZZConfigVersion.cmake', 'cmake/FBZZ/FBZZTargets.cmake',
      'cmake/FBZZ/build.config.in', 'cmake/FBZZ/FBZZAgilitySDK.cmake', 'cmake/FBZZ/AgilitySDKExports.cpp.in',
      'cmake/FBZZ/StageAgilitySDK.cmake', 'include/Engine/Scene/ScriptDllAbi.hpp',
      'include/Graphics/Renderer/RenderScene.hpp', 'include/Physics/World.hpp', 'include/Fluid/FluidSolver.hpp',
      'include/Math/Vector3.hpp', 'include/Core/Logger.hpp', 'share/fbzz/Assets/Shaders',
      `bin/${configuration}/published.stamp`, `lib/${configuration}/imgui.lib`, `bin/${configuration}/imgui.dll`,
      `tools/${configuration}/Editor/FBZZEditor.exe`, `tools/${configuration}/Editor/imgui.dll`,
    ];
    for (const library of SDK_EDITOR_LIBRARIES) {
      requiredPaths.push(`lib/${configuration}/FBZZ${library}.lib`, `bin/${configuration}/FBZZ${library}.dll`,
        `tools/${configuration}/Editor/FBZZ${library}.dll`);
    }
    for (const name of GetSdkEditorDlls(configuration)) {
      requiredPaths.push(`bin/${configuration}/${name}`, `tools/${configuration}/Editor/${name}`);
      const binHash = asString(files?.[`bin/${configuration}/${name}`]);
      if (!/^[0-9a-f]{64}$/.test(binHash) || files?.[`tools/${configuration}/Editor/${name}`] !== binHash) return false;
    }
    if (runtime.dx12_enabled) {
      if (!Number.isInteger(runtime.agility_sdk_version) || Number(runtime.agility_sdk_version) <= 0
          || runtime.agility_path !== '.\\D3D12\\' || !asString(runtime.core_file_version)
          || !asString(runtime.agility_package) || !asString(runtime.dxc_version)) return false;
      for (const field of ['core', 'layers', 'dxc_exe', 'dxc_compiler', 'dxc_validator']) {
        if (!/^[0-9a-f]{64}$/.test(asString(runtime[`${field}_sha256`]))) return false;
      }
      for (const prefix of [`bin/${configuration}`, `tools/${configuration}/Editor`]) {
        requiredPaths.push(`${prefix}/D3D12/D3D12Core.dll`, `${prefix}/dxcompiler.dll`, `${prefix}/dxil.dll`, `${prefix}/WinPixEventRuntime.dll`);
        if (configuration !== 'Release') requiredPaths.push(`${prefix}/D3D12/d3d12SDKLayers.dll`);
        else if (await exists(path.join(candidate, prefix, 'D3D12/d3d12SDKLayers.dll'))) return false;
      }
      for (const name of ['dxc.exe', 'dxcompiler.dll', 'dxil.dll']) requiredPaths.push(`tools/${configuration}/DXC/${name}`);
      for (const name of ['LICENSE', 'LICENSE.txt', 'LICENSE-CODE.txt', 'VERSION', 'distributable files.txt']) requiredPaths.push(`share/fbzz/licenses/AgilitySDK/${name}`);
      for (const name of ['LICENSE', 'LICENCE-MIT.txt', 'LICENSE-LLVM.txt', 'LICENSE-MS.txt', 'VERSION']) requiredPaths.push(`share/fbzz/licenses/DXC/${name}`);
      for (const name of ['LICENSE', 'VERSION', 'ThirdPartyNotices.txt']) requiredPaths.push(`share/fbzz/licenses/WinPixEventRuntime/${name}`);
      for (const relativePath of [...requiredPaths]) {
        if (relativePath.startsWith('share/fbzz/licenses/')) requiredPaths.push(`tools/${configuration}/Editor/EngineLicenses/${relativePath.slice('share/fbzz/licenses/'.length)}`);
      }
      for (const prefix of [`bin/${configuration}`, `tools/${configuration}/Editor`]) {
        for (const [relativeFile, field] of [['D3D12/D3D12Core.dll', 'core'], ['dxcompiler.dll', 'dxc_compiler'], ['dxil.dll', 'dxc_validator']]) {
          if (files?.[`${prefix}/${relativeFile}`] !== runtime[`${field}_sha256`]) return false;
        }
        if (configuration !== 'Release' && files?.[`${prefix}/D3D12/d3d12SDKLayers.dll`] !== runtime.layers_sha256) return false;
      }
      for (const [name, field] of [['dxc.exe', 'dxc_exe'], ['dxcompiler.dll', 'dxc_compiler'], ['dxil.dll', 'dxc_validator']]) {
        if (files?.[`tools/${configuration}/DXC/${name}`] !== runtime[`${field}_sha256`]) return false;
      }
      const agilityVersion = await readFile(path.join(candidate, 'share/fbzz/licenses/AgilitySDK/VERSION'), 'utf8');
      const dxcVersion = await readFile(path.join(candidate, 'share/fbzz/licenses/DXC/VERSION'), 'utf8');
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
        if (!asString(source).includes(`set(${asString(name)} "${asString(value)}")`)) return false;
      }
      if (!agilityVersion.includes(`set(FBZZ_AGILITY_SDK_VERSION ${String(runtime.agility_sdk_version)})`)) return false;
    }
    const checks = await Promise.all(requiredPaths.map(async (relativePath) => {
      const filePath = path.join(candidate, relativePath);
      if (!await exists(filePath)) return false;
      const info = await stat(filePath);
      return relativePath === 'share/fbzz/Assets/Shaders'
        ? info.isDirectory()
        : info.isFile() && /^[0-9a-f]{64}$/.test(asString(files?.[relativePath]));
    }));
    return checks.every(Boolean);
  } catch {
    return false;
  }
}

/// @brief 起動直前に Editor の DLL 一式を公開 SDK と SHA256 で照合する。
/// @note SDK 探索では metadata だけを確認し、実ファイル走査は起動境界で行う。
/// @see Docs/design/shared-engine-sdk.md
export async function AssertSdkEditorRuntime(sdkRoot: string, configuration: SdkBuildConfiguration): Promise<void> {
  if (!await isSdkRoot(sdkRoot, configuration)) {
    throw new Error(`SDK ${configuration}は未公開、不完全、または Editor と SDK の DLL が一致しません。SDKを再公開してください。`);
  }
  const document = parse(await readFile(path.join(sdkRoot, 'fbzz-sdk.toml'), 'utf8')) as Record<string, unknown>;
  const files = (document.files as Record<string, Record<string, unknown>> | undefined)?.[configuration];
  const runtime = document.runtime as Record<string, unknown> | undefined;
  const record = (document.configurations as Record<string, Record<string, unknown>> | undefined)?.[configuration];
  if (!files || record?.validated !== true || record.fingerprint !== runtime?.fingerprint) {
    throw new Error(`SDK ${configuration}の公開状態が変わりました。SDKの公開完了後に起動してください。`);
  }
  for (const name of GetSdkEditorDlls(configuration)) {
    if (!/^[0-9a-f]{64}$/.test(asString(files[`bin/${configuration}/${name}`]))
        || files[`bin/${configuration}/${name}`] !== files[`tools/${configuration}/Editor/${name}`]) {
      throw new Error(`SDK Editorと公開SDKのDLL記録が一致しません: ${name}`);
    }
  }
  await Promise.all(GetSdkEditorDlls(configuration).flatMap((name) => [
    `bin/${configuration}/${name}`, `tools/${configuration}/Editor/${name}`,
  ]).map(async (relativePath) => {
    const digest = createHash('sha256');
    for await (const chunk of createReadStream(path.join(sdkRoot, relativePath))) digest.update(chunk);
    if (digest.digest('hex') !== files[relativePath]) {
      throw new Error(`SDK EditorのDLLが公開時のSHA256と一致しません: ${relativePath}。SDKを再公開してください。`);
    }
  }));
}

/// @brief sdk_idから互換判定に使うEngine versionを取り出す。
/// @note ランタイム契約と構成の検証状態は、Engine の版とは別に照合する。
/// @note 旧方式のプロジェクトは`0.1.0-dev.<rev>`のような世代IDをpinしている。
/// @note 版が同じなら現行の`0.1.0`へ解決し、.fbzz_projを書き換えずに開ける。
function sdkVersionOf(sdkId: string): string {
  return sdkId.trim().split('-')[0] ?? '';
}

/// @brief manifestを正本として、ディレクトリ名に依存せずSDK IDを取得する。
async function readSdkId(sdkRoot: string, configuration: SdkBuildConfiguration = 'Development'): Promise<string> {
  if (!await isSdkRoot(sdkRoot, configuration)) return '';
  try {
    const doc = parse(await readFile(path.join(sdkRoot, 'fbzz-sdk.toml'), 'utf8')) as Record<string, unknown>;
    const sdk = (doc.sdk ?? {}) as Record<string, unknown>;
    return asString(sdk.id) || path.basename(path.resolve(sdkRoot));
  } catch {
    return '';
  }
}

function asSdkConfiguration(value: unknown): SdkBuildConfiguration {
  if (value === 'Debug' || value === 'Development' || value === 'Release') return value;
  /// @note 旧構成名は package の構成と独立して Development へ移す。
  if (value === 'dev' || value === 'development') return 'Development';
  return GAME_HUB_BUILD_CONFIGURATION;
}

const THEMES: readonly HubTheme[] = ['modern', 'dark', 'light', 'system'];

/// @note 'midnight' は旧テーマ名。既存の hub_config.toml を読み捨てず 'modern' へ寄せる。
function asTheme(value: unknown): HubTheme {
  return THEMES.includes(value as HubTheme) ? value as HubTheme : 'modern';
}

/// @brief SDK store から選択構成の公開時刻が最も新しい有効 SDK を返す。
/// @note 通常は SDK/<version>/ が一つだが、旧方式の世代も解決できる。
async function findLatestSdkInStore(storeRoot: string, preferredVersion = '', configuration: SdkBuildConfiguration = 'Development'): Promise<string> {
  try {
    const entries = (await readdir(storeRoot, { withFileTypes: true }))
      .filter((entry) => entry.isDirectory());

    /// @note 通常は現行SDKが `SDK/<ENGINE_VERSION>` にある。全候補のmanifestを検証する前に
    /// @note それだけを確認すると、旧世代SDKが多数残ったstoreでも起動時のI/Oを抑えられる。
    const preferredEntry = entries.find((entry) => entry.name === preferredVersion);
    if (preferredEntry) {
      const preferredRoot = path.join(storeRoot, preferredEntry.name);
      if (await isSdkRoot(preferredRoot, configuration)) return preferredRoot;
    }

    const candidates = await Promise.all(entries
      .filter((entry) => entry !== preferredEntry)
      .map(async (entry) => {
        const root = path.join(storeRoot, entry.name);
        if (!await isSdkRoot(root, configuration)) return null;
        return { root, modified: (await stat(path.join(root, 'bin', configuration, 'published.stamp'))).mtimeMs };
      }));
    return candidates
      .filter((candidate): candidate is { root: string; modified: number } => candidate !== null)
      .sort((left, right) => right.modified - left.modified)[0]?.root ?? '';
  } catch {
    return '';
  }
}

async function detectSdkRoot(configuredPath: string, configuration: SdkBuildConfiguration): Promise<string> {
  const configured = configuredPath ? path.resolve(configuredPath) : '';
  const configuredVersion = configured ? path.join(configured, 'SDK', ENGINE_VERSION) : '';
  if (configured) {
    /// @note 設定済みパスと、その直下の`SDK/<version>/`は独立しているため同時に確認する。
    const [configuredValid, versionValid] = await Promise.all([
      isSdkRoot(configured, configuration),
      isSdkRoot(configuredVersion, configuration),
    ]);
    if (configuredValid) return configured;
    if (versionValid) return configuredVersion;
    const configuredStoreSdk = await findLatestSdkInStore(configured, '', configuration);
    if (configuredStoreSdk) return configuredStoreSdk;
  }

  /// @note 各起点を一つずつ最上位まで走査すると、存在しないパスへのaccessを最大24段直列に待つ。
  /// @note 同じ深さの候補を並列確認し、最寄りのSDKが見つかった時点で終了する。
  let currentRoots = [...new Set(
    [process.cwd(), app.getAppPath(), path.dirname(process.execPath)].map((start) => path.resolve(start)),
  )];
  for (let depth = 0; depth < 8 && currentRoots.length > 0; depth += 1) {
    const stores = [...new Set(currentRoots.map((current) => path.join(current, 'SDK')))];
    const candidates = await Promise.all(stores.map((store) => findLatestSdkInStore(store, ENGINE_VERSION, configuration)));
    const found = candidates.find(Boolean);
    if (found) return found;

    const parents = currentRoots
      .map((current) => path.dirname(current))
      .filter((parent, index) => parent !== currentRoots[index]);
    currentRoots = [...new Set(parents)];
  }
  return '';
}

export class ConfigStore {
  private config: HubConfig = structuredClone(DEFAULT_CONFIG);
  private sdkResolutionPromise: Promise<void> = Promise.resolve();
  private configGeneration = 0;

  private get configPath(): string {
    return path.join(app.getPath('appData'), 'FBZZHub', 'hub_config.toml');
  }

  async load(): Promise<HubConfig> {
    await mkdir(path.dirname(this.configPath), { recursive: true });
    let shouldSave = false;

    try {
      const document = parse(await readFile(this.configPath, 'utf8')) as Record<string, unknown>;
      const hub = (document.hub ?? {}) as Record<string, unknown>;
      const update = (document.update ?? {}) as Record<string, unknown>;
      const rawProjects = Array.isArray(document.projects) ? document.projects : [];

      this.config = {
        settings: {
          editorExe: asString(hub.editor_exe),
          sdkRoot: asString(hub.sdk_root) || asString(hub.engine_root),
          sdkId: asString(hub.sdk_id),
          sdkConfiguration: asSdkConfiguration(hub.sdk_configuration),
          theme: asTheme(hub.theme),
          checkForUpdates: update.check !== false,
        },
        update: {
          lastCheckedAt: asString(update.last_checked_at),
          latestVersion: asString(update.latest_version),
          latestUrl: asString(update.latest_url),
          latestNotes: asString(update.latest_notes),
          dismissedVersion: asString(update.dismissed_version),
        },
        projects: rawProjects.flatMap((item) => {
          if (!item || typeof item !== 'object') return [];
          const table = item as Record<string, unknown>;
          const projectPath = asString(table.path);
          return projectPath
            ? [{ path: normalizeProjectPath(projectPath), lastOpened: asString(table.last_opened) }]
            : [];
        }),
      };

    } catch (error) {
      /// @note 初回起動時のファイル不存在は正常系。壊れた既存設定は上書きせず既定値で起動する。
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') {
        console.warn('hub_config.tomlを読み込めませんでした。', error);
      }
      this.config = structuredClone(DEFAULT_CONFIG);
      shouldSave = true;
    }

    if (shouldSave) await this.save();

    /// @note SDK探索はネットワークドライブや大量の旧SDKを含むと遅くなるため、設定ファイルの
    /// @note 読み込みとは分離する。GameHubは保存済み設定で即座に画面を出し、探索結果は後から反映する。
    const generation = this.configGeneration;
    this.sdkResolutionPromise = this.resolveSdkInBackground(generation).catch((error: unknown) => {
      /// @note 自動検出は補助機能なので、失敗してもGameHubの画面表示や手動設定を止めない。
      console.warn('SDKの自動検出に失敗しました。', error);
    });

    return this.snapshot();
  }

  /// @brief 起動時に開始したSDK探索が必要な操作だけ完了を待つ。
  async ensureSdkResolved(): Promise<void> {
    await this.sdkResolutionPromise;
  }

  snapshot(): HubConfig {
    return structuredClone(this.config);
  }

  async setSettings(settings: HubSettings): Promise<void> {
    /// @note ユーザーの手動設定を、起動時に残っている自動検出結果で上書きしない。
    this.configGeneration += 1;
    const sdkRoot = settings.sdkRoot.trim();
    const configuration = asSdkConfiguration(settings.sdkConfiguration);
    const sdkId = await readSdkId(sdkRoot, configuration);
    if (sdkRoot && !sdkId) throw new Error(`選択されたSDKの${configuration}構成は不完全、未公開、または現在のランタイム契約と不一致です。SDKを再公開してください。`);
    this.config.settings = {
      editorExe: settings.editorExe.trim(),
      sdkRoot,
      sdkId,
      sdkConfiguration: asSdkConfiguration(settings.sdkConfiguration),
      theme: asTheme(settings.theme),
      checkForUpdates: settings.checkForUpdates !== false,
    };
    await this.save();
  }

  /// @brief 新しい版の問い合わせ結果を差し替えて保存する。
  async setUpdateState(update: UpdateState): Promise<void> {
    this.config.update = { ...update };
    await this.save();
  }

  /// @brief プロジェクトのSDK IDを、選択中SDKまたは同じstore内の同versionのSDKへ解決する。
  async resolveSdkRoot(sdkId: string): Promise<string> {
    await this.ensureSdkResolved();
    const selected = this.config.settings.sdkRoot;
    const configuration = this.config.settings.sdkConfiguration;
    const requestedVersion = sdkVersionOf(sdkId || this.config.settings.sdkId);
    const selectedId = await readSdkId(selected, configuration);
    if (selectedId && (!requestedVersion || sdkVersionOf(selectedId) === requestedVersion)) return selected;

    if (selected && requestedVersion) {
      const sibling = path.join(path.dirname(path.resolve(selected)), requestedVersion);
      if (sdkVersionOf(await readSdkId(sibling, configuration)) === requestedVersion) return sibling;
    }
    return '';
  }

  async addProject(projectPath: string, lastOpened = ''): Promise<void> {
    const normalized = normalizeProjectPath(projectPath);
    const existing = this.config.projects.find((project) => project.path === normalized);
    if (existing) {
      existing.lastOpened = lastOpened || existing.lastOpened;
    } else {
      this.config.projects.push({ path: normalized, lastOpened });
    }
    await this.save();
  }

  async removeProject(projectPath: string): Promise<void> {
    const normalized = normalizeProjectPath(projectPath);
    this.config.projects = this.config.projects.filter((project) => project.path !== normalized);
    await this.save();
  }

  async touchProject(projectPath: string): Promise<string> {
    const lastOpened = new Date().toISOString();
    await this.addProject(projectPath, lastOpened);
    return lastOpened;
  }

  private async resolveSdkInBackground(generation: number): Promise<void> {
    const configuredRoot = this.config.settings.sdkRoot;
    const detectedSdkRoot = await detectSdkRoot(configuredRoot, this.config.settings.sdkConfiguration);
    if (generation !== this.configGeneration) return;

    let changed = false;
    if (detectedSdkRoot !== this.config.settings.sdkRoot) {
      this.config.settings.sdkRoot = detectedSdkRoot;
      changed = true;
    }

    const detectedSdkId = await readSdkId(this.config.settings.sdkRoot, this.config.settings.sdkConfiguration);
    if (generation !== this.configGeneration) return;
    if (detectedSdkId !== this.config.settings.sdkId) {
      this.config.settings.sdkId = detectedSdkId;
      changed = true;
    }

    if (changed) await this.save();
  }

  private async save(): Promise<void> {
    const document = {
      hub: {
        editor_exe: this.config.settings.editorExe,
        sdk_root: this.config.settings.sdkRoot,
        sdk_id: this.config.settings.sdkId,
        sdk_configuration: this.config.settings.sdkConfiguration,
        theme: this.config.settings.theme,
      },
      update: {
        check: this.config.settings.checkForUpdates,
        last_checked_at: this.config.update.lastCheckedAt,
        latest_version: this.config.update.latestVersion,
        latest_url: this.config.update.latestUrl,
        latest_notes: this.config.update.latestNotes,
        dismissed_version: this.config.update.dismissedVersion,
      },
      projects: this.config.projects.map((project) => ({
        path: project.path,
        last_opened: project.lastOpened,
      })),
    };

    await writeFile(this.configPath, stringify(document), 'utf8');
  }
}
