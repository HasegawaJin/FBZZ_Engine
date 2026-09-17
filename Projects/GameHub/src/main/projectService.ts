/**
 * @file projectService.ts
 * @brief プロジェクト検証、サムネイル取得、Editor 起動を OS 権限側へ集約する。
 * @author Hasegawa Jin
 * @date 2026/07/19
 */

import { shell } from 'electron';
import { access, readFile } from 'node:fs/promises';
import path from 'node:path';
import { execFile, spawn, type SpawnOptions } from 'node:child_process';
import { promisify } from 'node:util';
import { parse } from 'smol-toml';
import { ENGINE_VERSION, type HubSettings, type ProjectEntry } from '../shared/contracts';
import type { ConfigProject } from './configStore';

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

export class ProjectService {
  // 起動処理中のプロジェクト。spawn が返るまでの連打を弾く。
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

    // 独立した検証I/Oを同時に開始し、プロジェクトごとの直列待ちをなくす。
    const [cmakeExists, layoutChecks, generatedChecks, thumbnailDataUrl, projectFile] = await Promise.all([
      exists(path.join(projectRoot, 'CMakeLists.txt')),
      Promise.all(['Assets', 'Src', 'Include'].map((name) => exists(path.join(projectRoot, name)))),
      Promise.all(['Lib', 'Binaries', 'Build'].map((name) => exists(path.join(projectRoot, name)))),
      this.loadThumbnail(projectRoot),
      readFile(path.join(projectRoot, '.fbzz_proj'), 'utf8').catch(() => null),
    ]);
    empty.cmakeExists = cmakeExists;
    empty.layoutValid = layoutChecks.every(Boolean);
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
      empty.engineVersionMismatch = empty.engineVersion !== '-' && empty.engineVersion !== ENGINE_VERSION;
      empty.migrationRequired = empty.engineVersionMismatch || !empty.sdkId;
    } catch {
      // 壊れたプロジェクトも一覧から消さず、修復できるよう状態として返す。
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
    const projectRoot = path.resolve(projectPath);
    if (this.launching.has(projectRoot)) throw new Error('このプロジェクトは起動処理中です。');
    this.launching.add(projectRoot);
    try {
      if (!sdkRoot) throw new Error('プロジェクトが要求するFBZZ SDKがSDK storeにありません。');
      const editorPath = await this.resolveEditorPath(settings, sdkRoot);
      if (!editorPath) throw new Error(`SDK ${settings.sdkConfiguration}用FBZZEditor.exeが見つかりません。`);
      // WHY: 同じプロジェクトを 2 つの Editor で開くと .meta とシーンの書き換えが競合する。
      if (await this.isOpenInEditor(editorPath, projectRoot)) {
        throw new Error(`${path.basename(projectRoot)} は既に Editor で開いています。先に閉じてください。`);
      }
      await this.spawnEditor(editorPath, projectRoot, sdkRoot);
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
        // Editor・CMake・AssetManagerへ同じimmutable SDKを渡し、ソースツリー探索を禁止する。
        env: {
          ...process.env,
          FBZZ_SDK_ROOT: sdkRoot,
          FBZZ_ENGINE_ASSET_ROOT: path.join(sdkRoot, 'share', 'fbzz', 'Assets'),
          // Shared SDK は immutable artifact であり、Engine ソースの鮮度判定・再ビルド対象ではない。
          FBZZ_SKIP_ENGINE_REBUILD: '1',
        },
      };
      // WHY: Windows では短命の start ブローカーに Editor を生成させ、GameHub が
      //      Editor の直接の親・lifetime owner にならないようにする。
      //      shell オプションは使わず引数配列で渡し、プロジェクトパスをコマンドとして解釈させない。
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

  /**
   * 同じプロジェクトを `--project` で開いている Editor プロセスがあるか。
   * Windows 以外と問い合わせ失敗時は false (起動を止める根拠が無いので通す)。
   */
  private async isOpenInEditor(editorPath: string, projectRoot: string): Promise<boolean> {
    if (process.platform !== 'win32') return false;
    const executableName = path.basename(editorPath).replaceAll("'", "''");
    // 非 ASCII のプロジェクトパスが OEM コードページで化けて照合に失敗しないよう UTF-8 で受ける。
    const script = '[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; '
      + `Get-CimInstance Win32_Process -Filter "Name='${executableName}'" | ForEach-Object { $_.CommandLine }`;
    try {
      const { stdout } = await execFileAsync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], {
        windowsHide: true,
        timeout: 5_000,
        encoding: 'utf8',
      });
      // 前後を空白か引用符で区切り、`C:\Games\Foo` が `C:\Games\FooBar` に一致しないようにする。
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
    const candidates = [
      settings.editorExe,
      path.join(sdkRoot, 'tools', settings.sdkConfiguration, 'Editor', 'FBZZEditor.exe'),
    ].filter(Boolean);
    for (const candidate of candidates) {
      if (await exists(candidate)) return candidate;
    }
    return null;
  }
}
