// FBZZ GameHub
// configStore.ts | main
// C++版と共通のhub_config.tomlを読み書きする永続化層

import { app } from 'electron';
import { access, mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { parse, stringify } from 'smol-toml';
import { ENGINE_VERSION, type HubSettings, type HubTheme } from '../shared/contracts';

export interface ConfigProject {
  path: string;
  lastOpened: string;
}

interface HubConfig {
  settings: HubSettings;
  projects: ConfigProject[];
}

const DEFAULT_CONFIG: HubConfig = {
  settings: { editorExe: '', sdkRoot: '', theme: 'midnight' },
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

async function detectSdkRoot(configuredPath: string): Promise<string> {
  const configured = configuredPath ? path.resolve(configuredPath) : '';
  const configuredVersion = configured ? path.join(configured, 'SDK', ENGINE_VERSION) : '';
  if (configured) {
    // 設定済みパスと旧Engineルートからの移行候補は独立しているため同時に確認する。
    const [configuredValid, versionValid] = await Promise.all([
      isSdkRoot(configured),
      isSdkRoot(configuredVersion),
    ]);
    if (configuredValid) return configured;
    if (versionValid) return configuredVersion;
  }

  // 各起点を一つずつ最上位まで走査すると、存在しないパスへのaccessを最大24段直列に待つ。
  // 同じ深さの候補を並列確認し、最寄りのSDKが見つかった時点で終了する。
  let currentRoots = [...new Set(
    [process.cwd(), app.getAppPath(), path.dirname(process.execPath)].map((start) => path.resolve(start)),
  )];
  for (let depth = 0; depth < 8 && currentRoots.length > 0; depth += 1) {
    const candidates = [...new Set(currentRoots.map((current) => path.join(current, 'SDK', ENGINE_VERSION)))];
    const validity = await Promise.all(candidates.map(isSdkRoot));
    const foundIndex = validity.findIndex(Boolean);
    if (foundIndex >= 0) return candidates[foundIndex] ?? configured;

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
    } catch (error) {
      // 初回起動時のファイル不存在は正常系。壊れた既存設定は上書きせず既定値で起動する。
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') {
        console.warn('hub_config.tomlを読み込めませんでした。', error);
      }
      this.config = structuredClone(DEFAULT_CONFIG);
      const detectedSdkRoot = await detectSdkRoot('');
      if (detectedSdkRoot) this.config.settings.sdkRoot = detectedSdkRoot;
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
    this.config.settings = {
      editorExe: settings.editorExe.trim(),
      sdkRoot: settings.sdkRoot.trim(),
      theme,
    };
    await this.save();
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
