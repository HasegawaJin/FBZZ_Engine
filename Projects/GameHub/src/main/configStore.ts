// FBZZ GameHub
// configStore.ts | main
// C++版と共通のhub_config.tomlを読み書きする永続化層

import { app } from 'electron';
import { access, mkdir, readFile, readdir, stat, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { parse, stringify } from 'smol-toml';
import {
  ENGINE_VERSION,
  type HubSettings,
  type HubTheme,
  type SdkBuildConfiguration,
} from '../shared/contracts';

export interface ConfigProject {
  path: string;
  lastOpened: string;
}

interface HubConfig {
  settings: HubSettings;
  projects: ConfigProject[];
}

const DEFAULT_CONFIG: HubConfig = {
  settings: {
    editorExe: '',
    sdkRoot: '',
    sdkId: '',
    sdkConfiguration: 'Development',
    theme: 'midnight',
  },
  projects: [],
};

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

async function isSdkRoot(candidate: string): Promise<boolean> {
  if (!candidate) return false;
  const checks = [
    path.join(candidate, 'fbzz-sdk.toml'),
    path.join(candidate, 'cmake', 'FBZZ', 'FBZZConfig.cmake'),
    path.join(candidate, 'include', 'Engine'),
  ];
  return (await Promise.all(checks.map(exists))).every(Boolean);
}

/**
 * sdk_idから互換判定に使うEngine versionを取り出す。
 * SDKはversionごとに1つだけ公開されるため、versionが一致すれば同じSDKを指す。
 * WHY: 旧方式のプロジェクトは`0.1.0-dev.<rev>`のような世代IDをpinしている。
 *      版が同じなら現行の`0.1.0`へ解決し、.fbzz_projを書き換えずに開けるようにする。
 */
function sdkVersionOf(sdkId: string): string {
  return sdkId.trim().split('-')[0] ?? '';
}

/** manifestを正本として、ディレクトリ名に依存せずSDK IDを取得する。 */
async function readSdkId(sdkRoot: string): Promise<string> {
  if (!await isSdkRoot(sdkRoot)) return '';
  try {
    const doc = parse(await readFile(path.join(sdkRoot, 'fbzz-sdk.toml'), 'utf8')) as Record<string, unknown>;
    const sdk = (doc.sdk ?? {}) as Record<string, unknown>;
    return asString(sdk.id) || path.basename(path.resolve(sdkRoot));
  } catch {
    return '';
  }
}

function asSdkConfiguration(value: unknown): SdkBuildConfiguration {
  return value === 'Debug' || value === 'Release' ? value : 'Development';
}

/**
 * SDK store直下から、manifest更新時刻が最も新しいSDKを返す。
 * 通常は`SDK/<version>/`が1つだけ存在するが、旧方式の世代が残っていても最新を選べる。
 */
async function findLatestSdkInStore(storeRoot: string): Promise<string> {
  try {
    const candidates = await Promise.all((await readdir(storeRoot, { withFileTypes: true }))
      .filter((entry) => entry.isDirectory())
      .map(async (entry) => {
        const root = path.join(storeRoot, entry.name);
        if (!await isSdkRoot(root)) return null;
        return { root, modified: (await stat(path.join(root, 'fbzz-sdk.toml'))).mtimeMs };
      }));
    return candidates
      .filter((candidate): candidate is { root: string; modified: number } => candidate !== null)
      .sort((left, right) => right.modified - left.modified)[0]?.root ?? '';
  } catch {
    return '';
  }
}

async function detectSdkRoot(configuredPath: string): Promise<string> {
  const configured = configuredPath ? path.resolve(configuredPath) : '';
  const configuredVersion = configured ? path.join(configured, 'SDK', ENGINE_VERSION) : '';
  if (configured) {
    // 設定済みパスと、その直下の`SDK/<version>/`は独立しているため同時に確認する。
    const [configuredValid, versionValid] = await Promise.all([
      isSdkRoot(configured),
      isSdkRoot(configuredVersion),
    ]);
    if (configuredValid) return configured;
    if (versionValid) return configuredVersion;
    const configuredStoreSdk = await findLatestSdkInStore(configured);
    if (configuredStoreSdk) return configuredStoreSdk;
  }

  // 各起点を一つずつ最上位まで走査すると、存在しないパスへのaccessを最大24段直列に待つ。
  // 同じ深さの候補を並列確認し、最寄りのSDKが見つかった時点で終了する。
  let currentRoots = [...new Set(
    [process.cwd(), app.getAppPath(), path.dirname(process.execPath)].map((start) => path.resolve(start)),
  )];
  for (let depth = 0; depth < 8 && currentRoots.length > 0; depth += 1) {
    const stores = [...new Set(currentRoots.map((current) => path.join(current, 'SDK')))];
    const candidates = await Promise.all(stores.map(findLatestSdkInStore));
    const found = candidates.find(Boolean);
    if (found) return found;

    const parents = currentRoots
      .map((current) => path.dirname(current))
      .filter((parent, index) => parent !== currentRoots[index]);
    currentRoots = [...new Set(parents)];
  }
  return configured;
}

export class ConfigStore {
  private config: HubConfig = structuredClone(DEFAULT_CONFIG);

  private get configPath(): string {
    return path.join(app.getPath('appData'), 'FBZZHub', 'hub_config.toml');
  }

  async load(): Promise<HubConfig> {
    await mkdir(path.dirname(this.configPath), { recursive: true });
    let shouldSave = false;

    try {
      const document = parse(await readFile(this.configPath, 'utf8')) as Record<string, unknown>;
      const hub = (document.hub ?? {}) as Record<string, unknown>;
      const rawProjects = Array.isArray(document.projects) ? document.projects : [];
      const theme = asString(hub.theme);

      this.config = {
        settings: {
          editorExe: asString(hub.editor_exe),
          sdkRoot: asString(hub.sdk_root) || asString(hub.engine_root),
          sdkId: asString(hub.sdk_id),
          sdkConfiguration: asSdkConfiguration(hub.sdk_configuration),
          theme: theme === 'dark' ? 'dark' : 'midnight',
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

      const detectedSdkRoot = await detectSdkRoot(this.config.settings.sdkRoot);
      if (detectedSdkRoot && detectedSdkRoot !== this.config.settings.sdkRoot) {
        this.config.settings.sdkRoot = detectedSdkRoot;
        shouldSave = true;
      }
      const detectedSdkId = await readSdkId(this.config.settings.sdkRoot);
      if (detectedSdkId && detectedSdkId !== this.config.settings.sdkId) {
        this.config.settings.sdkId = detectedSdkId;
        shouldSave = true;
      }
    } catch (error) {
      // 初回起動時のファイル不存在は正常系。壊れた既存設定は上書きせず既定値で起動する。
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') {
        console.warn('hub_config.tomlを読み込めませんでした。', error);
      }
      this.config = structuredClone(DEFAULT_CONFIG);
      const detectedSdkRoot = await detectSdkRoot('');
      if (detectedSdkRoot) {
        this.config.settings.sdkRoot = detectedSdkRoot;
        this.config.settings.sdkId = await readSdkId(detectedSdkRoot);
      }
      shouldSave = true;
    }

    if (shouldSave) await this.save();

    return this.snapshot();
  }

  snapshot(): HubConfig {
    return structuredClone(this.config);
  }

  async setSettings(settings: HubSettings): Promise<void> {
    const theme: HubTheme = settings.theme === 'dark' ? 'dark' : 'midnight';
    const sdkRoot = settings.sdkRoot.trim();
    const sdkId = await readSdkId(sdkRoot);
    if (sdkRoot && !sdkId) throw new Error('選択されたフォルダーは有効なFBZZ SDKではありません。');
    this.config.settings = {
      editorExe: settings.editorExe.trim(),
      sdkRoot,
      sdkId,
      sdkConfiguration: asSdkConfiguration(settings.sdkConfiguration),
      theme,
    };
    await this.save();
  }

  /** プロジェクトのSDK IDを、選択中SDKまたは同じstore内の同versionのSDKへ解決する。 */
  async resolveSdkRoot(sdkId: string): Promise<string> {
    const selected = this.config.settings.sdkRoot;
    const requestedVersion = sdkVersionOf(sdkId || this.config.settings.sdkId);
    if (selected && (!requestedVersion || sdkVersionOf(await readSdkId(selected)) === requestedVersion)) return selected;

    if (selected && requestedVersion) {
      const sibling = path.join(path.dirname(path.resolve(selected)), requestedVersion);
      if (sdkVersionOf(await readSdkId(sibling)) === requestedVersion) return sibling;
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

  private async save(): Promise<void> {
    const document = {
      hub: {
        editor_exe: this.config.settings.editorExe,
        sdk_root: this.config.settings.sdkRoot,
        sdk_id: this.config.settings.sdkId,
        sdk_configuration: this.config.settings.sdkConfiguration,
        theme: this.config.settings.theme,
      },
      projects: this.config.projects.map((project) => ({
        path: project.path,
        last_opened: project.lastOpened,
      })),
    };

    await writeFile(this.configPath, stringify(document), 'utf8');
  }
}
