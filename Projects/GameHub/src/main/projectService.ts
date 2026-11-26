/// @file    projectService.ts
/// @brief プロジェクト検証、サムネイル取得、Editor 起動を OS 権限側へ集約する。
/// @author  Hasegawa Jin
/// @date    2026-07-19

import { shell } from 'electron';
import { access, readFile, stat } from 'node:fs/promises';
import path from 'node:path';
import { execFile, spawn, type SpawnOptions } from 'node:child_process';
import { promisify } from 'node:util';
import { parse } from 'smol-toml';
import { ENGINE_VERSION, GetSdkEditorPath, type HubSettings, type ProjectEntry } from '../shared/contracts';
import { AssertSdkEditorRuntime, type ConfigProject } from './configStore';

const execFileAsync = promisify(execFile);

async function exists(target: string): Promise<boolean> {
  try {
    await access(target);
    return true;
  } catch {
    return false;
  }
}

function stringField(table: Record<string, unknown>, key: string, fallback = ''): string {
  return typeof table[key] === 'string' ? table[key] as string : fallback;
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/// @note 移動前の絶対パスは Editor の ProjectResolver と同じく、末尾からプロジェクト配下を探す。
async function ResolveProjectFile(projectRoot: string, filePath: string): Promise<string> {
  const absolute = path.resolve(projectRoot, filePath);
  if (!path.isAbsolute(filePath) || await exists(absolute)) {
    return absolute;
  }

  // 移動元の絶対パスが失効しているときは、短い末尾から順に移動後のルートへ足す。
  let remainingPath = filePath;
  let suffix = '';
  while (remainingPath !== path.dirname(remainingPath)) {
    suffix = path.join(path.basename(remainingPath), suffix);
    const candidate = path.resolve(projectRoot, suffix);
    if (await exists(candidate)) {
      return candidate;
    }
    remainingPath = path.dirname(remainingPath);
  }
  return absolute;
}

/// @note C# authoring の起動可否は設定とシーンで決まり、native 用 Include / Src は不要。
async function HasLaunchFiles(projectRoot: string, project: Record<string, unknown>): Promise<boolean> {
  // Sandbox が直接利用する雛形では未展開の設定パスを、既定の配置に解決する。
  let settingsPath = stringField(project, 'settings_path');
  if (settingsPath.startsWith('{{')) {
    settingsPath = 'ProjectSettings/ProjectSettings.toml';
  }
  if (!settingsPath) {
    return false;
  }

  try {
    // 実行時の開始シーンを優先し、省略時は .fbzz_proj の既定シーンを使う。
    const settingsFile = await ResolveProjectFile(projectRoot, settingsPath);
    const settings = parse(await readFile(settingsFile, 'utf8')) as Record<string, unknown>;
    const scenePath = stringField((settings.runtime ?? {}) as Record<string, unknown>, 'start_scene')
      || stringField(project, 'default_scene');
    if (!scenePath) {
      return false;
    }

    // 起動時に必要な Assets ディレクトリとシーンファイルの型を確認する。
    const [assets, scene] = await Promise.all([
      stat(path.join(projectRoot, 'Assets')),
      stat(await ResolveProjectFile(projectRoot, scenePath)),
    ]);
    return assets.isDirectory() && scene.isFile();
  } catch {
    return false;
  }
}

export class ProjectService {
  /// @note spawn 完了まで同じプロジェクトの起動要求を受け付けない。
  private readonly launching = new Set<string>();

  createPendingProjects(configProjects: ConfigProject[]): ProjectEntry[] {
    return configProjects
      .map((project) => {
        const projectRoot = path.resolve(project.path);
        return {
          name: path.basename(projectRoot),
          projectId: '',
          path: projectRoot,
          engineVersion: '-',
          sdkId: '',
          lastOpened: project.lastOpened,
          thumbnailDataUrl: '',
          pathExists: false,
          projectFileValid: false,
          cmakeExists: false,
          layoutValid: false,
          generatedRootsExist: false,
          engineVersionMismatch: false,
          migrationRequired: false,
          validationPending: true,
        };
      })
      .sort((left, right) => right.lastOpened.localeCompare(left.lastOpened));
  }

  async inspectProjects(configProjects: ConfigProject[]): Promise<ProjectEntry[]> {
    const projects = await Promise.all(configProjects.map((project) => this.inspectProject(project)));
    return projects.sort((left, right) => right.lastOpened.localeCompare(left.lastOpened));
  }

  async inspectProject(configProject: ConfigProject): Promise<ProjectEntry> {
    const projectRoot = path.resolve(configProject.path);
    const empty: ProjectEntry = {
      name: path.basename(projectRoot), projectId: '', path: projectRoot, engineVersion: '-', sdkId: '', lastOpened: configProject.lastOpened,
      thumbnailDataUrl: '', pathExists: false, projectFileValid: false, cmakeExists: false, layoutValid: false,
      generatedRootsExist: false, engineVersionMismatch: false, migrationRequired: false, validationPending: false,
    };
    if (!await exists(projectRoot)) return empty;
    empty.pathExists = true;

    const [cmakeExists, generatedChecks, thumbnailDataUrl, projectFile] = await Promise.all([
      exists(path.join(projectRoot, 'CMakeLists.txt')),
      Promise.all(['Binaries', 'Build'].map((name) => exists(path.join(projectRoot, name)))),
      this.loadThumbnail(projectRoot),
      readFile(path.join(projectRoot, '.fbzz_proj'), 'utf8').catch(() => null),
    ]);
    empty.cmakeExists = cmakeExists;
    empty.generatedRootsExist = generatedChecks.every(Boolean);
    empty.thumbnailDataUrl = thumbnailDataUrl;

    try {
      if (projectFile === null) return empty;
      const document = parse(projectFile) as Record<string, unknown>;
      const project = (document.project ?? {}) as Record<string, unknown>;
      const engine = (document.engine ?? {}) as Record<string, unknown>;
      empty.name = stringField(project, 'name', empty.name);
      empty.projectId = stringField(project, 'project_id');
      empty.engineVersion = stringField(project, 'engine_version', '-');
      empty.sdkId = stringField(engine, 'sdk_id');
      empty.projectFileValid = Boolean(empty.name && empty.projectId);
      empty.layoutValid = await HasLaunchFiles(projectRoot, project);
      empty.engineVersionMismatch = empty.engineVersion !== '-' && empty.engineVersion !== ENGINE_VERSION;
      empty.migrationRequired = empty.engineVersionMismatch || !empty.sdkId;
    } catch {
      // 壊れたプロジェクトも修復対象として一覧に残す。
    }
    return empty;
  }

  async readProjectSdkId(projectPath: string): Promise<string> {
    try {
      const document = parse(await readFile(path.join(projectPath, '.fbzz_proj'), 'utf8')) as Record<string, unknown>;
      return stringField((document.engine ?? {}) as Record<string, unknown>, 'sdk_id');
    } catch {
      return '';
    }
  }

  async openProject(projectPath: string, settings: HubSettings, sdkRoot: string): Promise<void> {
    // 起動処理中は、同じプロジェクトに対する別の起動要求を受け付けない。
    const projectRoot = path.resolve(projectPath);
    if (this.launching.has(projectRoot)) {
      throw new Error('このプロジェクトは起動処理中です。');
    }
    this.launching.add(projectRoot);

    try {
      // 指定された SDK 構成の Editor が利用できるか、起動前に調べる。
      if (!sdkRoot) {
        throw new Error('プロジェクトが要求するFBZZ SDKがSDK storeにありません。');
      }
      const editorPath = await this.resolveEditorPath(settings, sdkRoot);
      if (!editorPath) {
        throw new Error(`SDK ${settings.sdkConfiguration}用FBZZEditor.exeが見つかりません。同じ構成のSDKを公開してください。`);
      }

      // 開いている Editor との同時編集を避け、起動するバイナリが公開済みの組と一致することを確かめる。
      if (await this.isOpenInEditor(editorPath, projectRoot)) {
        throw new Error(`${path.basename(projectRoot)} は既に Editor で開いています。先に閉じてください。`);
      }
      await AssertSdkEditorRuntime(sdkRoot, settings.sdkConfiguration);
      await this.spawnEditor(editorPath, projectRoot, path.resolve(sdkRoot));
    } finally {
      this.launching.delete(projectRoot);
    }
  }

  async revealProject(projectPath: string): Promise<void> {
    const error = await shell.openPath(path.resolve(projectPath));
    if (error) throw new Error(error);
  }

  private spawnEditor(editorPath: string, projectRoot: string, sdkRoot: string): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const editorArgs = ['--project', projectRoot];
      const launchOptions: SpawnOptions = {
        detached: true,
        stdio: 'ignore',
        windowsHide: true,
        cwd: path.dirname(editorPath),
        // Editor・CMake・AssetManager は起動先と同じ SDK を使う。
        env: {
          ...process.env,
          FBZZ_SDK_ROOT: sdkRoot,
          FBZZ_ENGINE_ASSET_ROOT: path.join(sdkRoot, 'share', 'fbzz', 'Assets'),
          // Engine 更新は SDK 公開が担当し、部分的な source rebuild を起動しない。
          FBZZ_SKIP_ENGINE_REBUILD: '1',
        },
      };
      // Windows の start ブローカーで Editor の寿命を GameHub から切り離す。
      // shell オプションを使わず引数配列で渡す。
      const launcher = process.platform === 'win32'
        ? spawn(process.env.ComSpec ?? 'cmd.exe', ['/d', '/c', 'start', '', '/normal', editorPath, ...editorArgs], launchOptions)
        : spawn(editorPath, editorArgs, launchOptions);
      launcher.once('spawn', () => {
        launcher.unref();
        resolve();
      });
      launcher.once('error', (error) => {
        reject(new Error(`FBZZEditor.exeを起動できませんでした: ${error.message}`));
      });
    });
  }

  /// @return Windows 以外と問い合わせ失敗時は false。
  private async isOpenInEditor(editorPath: string, projectRoot: string): Promise<boolean> {
    if (process.platform !== 'win32') return false;
    const executableName = path.basename(editorPath).replaceAll("'", "''");
    // 非 ASCII のプロジェクトパスも UTF-8 で照合する。
    const script = '[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; '
      + `Get-CimInstance Win32_Process -Filter "Name='${executableName}'" | ForEach-Object { $_.CommandLine }`;
    try {
      const { stdout } = await execFileAsync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], {
        windowsHide: true,
        timeout: 5_000,
        encoding: 'utf8',
      });
      // プロジェクトパスの末尾も照合し、同じ接頭辞の別プロジェクトを除く。
      const pattern = new RegExp(`(^|[\\s"])${escapeRegExp(projectRoot)}("|\\s|$)`, 'i');
      return stdout.split(/\r?\n/).some((line) => pattern.test(line));
    } catch (error) {
      console.warn('起動中の Editor を確認できませんでした。', error);
      return false;
    }
  }

  private async loadThumbnail(projectRoot: string): Promise<string> {
    for (const candidate of [path.join(projectRoot, 'thumbnail.png'), path.join(projectRoot, 'Assets', 'thumbnail.png')]) {
      if (await exists(candidate)) {
        return `data:image/png;base64,${(await readFile(candidate)).toString('base64')}`;
      }
    }
    return '';
  }

  private async resolveEditorPath(settings: HubSettings, sdkRoot: string): Promise<string | null> {
    // legacy editorExe を使わず、解決済みプロジェクト SDK の構成だけを起動する。
    const editorPath = path.resolve(GetSdkEditorPath(sdkRoot, settings.sdkConfiguration));
    try { return (await stat(editorPath)).isFile() ? editorPath : null; } catch { return null; }
  }
}
