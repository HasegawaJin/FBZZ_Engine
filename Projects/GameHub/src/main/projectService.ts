// FBZZ GameHub
// projectService.ts | main
// プロジェクト検証、サムネイル取得、Editor起動をOS権限側へ集約する

import { app, shell } from 'electron';
import { access, readFile } from 'node:fs/promises';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { parse } from 'smol-toml';
import { ENGINE_VERSION, type HubSettings, type ProjectEntry } from '../shared/contracts';
import type { ConfigProject } from './configStore';

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

const EDITOR_BUILD_CONFIGURATIONS = ['Debug', 'Development', 'Release'] as const;

/** パッケージ版GameHubからリポジトリルートを遡り、利用可能なEditorビルドを探索する。 */
async function FindEditorInAncestors(start: string): Promise<string | null> {
  let current = path.resolve(start);
  for (let depth = 0; depth < 10; depth += 1) {
    for (const configuration of EDITOR_BUILD_CONFIGURATIONS) {
      const candidate = path.join(
        current,
        'build',
        configuration,
        'Binaries',
        configuration,
        'Editor',
        'FBZZEditor.exe',
      );
      if (await exists(candidate)) return candidate;
    }
    const parent = path.dirname(current);
    if (parent === current) break;
    current = parent;
  }
  return null;
}

export class ProjectService {
  createPendingProjects(configProjects: ConfigProject[]): ProjectEntry[] {
    return configProjects
      .map((project) => {
        const projectRoot = path.resolve(project.path);
        return {
          name: path.basename(projectRoot),
          projectId: '',
          path: projectRoot,
          engineVersion: '-',
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
      name: path.basename(projectRoot), projectId: '', path: projectRoot, engineVersion: '-', lastOpened: configProject.lastOpened,
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
      empty.name = stringField(project, 'name', empty.name);
      empty.projectId = stringField(project, 'project_id');
      empty.engineVersion = stringField(project, 'engine_version', '-');
      empty.projectFileValid = Boolean(empty.name && empty.projectId);
      empty.engineVersionMismatch = empty.engineVersion !== '-' && empty.engineVersion !== ENGINE_VERSION;
      empty.migrationRequired = empty.engineVersionMismatch;
    } catch {
      // 壊れたプロジェクトも一覧から消さず、修復できるよう状態として返す。
    }
    return empty;
  }

  async openProject(projectPath: string, settings: HubSettings): Promise<void> {
    const editorPath = await this.resolveEditorPath(settings);
    if (!editorPath) throw new Error('FBZZEditor.exeが見つかりません。Settingsでパスを指定してください。');
    await new Promise<void>((resolve, reject) => {
      const child = spawn(editorPath, ['--project', path.resolve(projectPath)], {
        detached: true,
        stdio: 'ignore',
        windowsHide: false,
        // 未展開の旧 .fbzz_proj でもEditorがSDKを特定してキャッシュ・シェーダーを移行できるよう渡す。
        env: { ...process.env, FBZZ_SDK_ROOT: settings.sdkRoot },
      });
      child.once('spawn', () => {
        child.unref();
        resolve();
      });
      child.once('error', (error) => {
        reject(new Error(`FBZZEditor.exeを起動できませんでした: ${error.message}`));
      });
    });
  }

  async revealProject(projectPath: string): Promise<void> {
    const error = await shell.openPath(path.resolve(projectPath));
    if (error) throw new Error(error);
  }

  private async loadThumbnail(projectRoot: string): Promise<string> {
    for (const candidate of [path.join(projectRoot, 'thumbnail.png'), path.join(projectRoot, 'Assets', 'thumbnail.png')]) {
      if (await exists(candidate)) {
        return `data:image/png;base64,${(await readFile(candidate)).toString('base64')}`;
      }
    }
    return '';
  }

  private async resolveEditorPath(settings: HubSettings): Promise<string | null> {
    const candidates = [
      settings.editorExe,
      settings.sdkRoot ? path.join(settings.sdkRoot, 'tools', 'Debug', 'Editor', 'FBZZEditor.exe') : '',
      settings.sdkRoot ? path.join(settings.sdkRoot, 'tools', 'Development', 'Editor', 'FBZZEditor.exe') : '',
      settings.sdkRoot ? path.join(settings.sdkRoot, 'tools', 'Release', 'Editor', 'FBZZEditor.exe') : '',
    ].filter(Boolean);
    for (const candidate of candidates) {
      if (await exists(candidate)) return candidate;
    }

    // WHY: out/<構成>/...から起動したGameHubではcwdもappPathもリポジトリルートではないため、親を探索する。
    for (const start of [process.cwd(), app.getAppPath(), path.dirname(process.execPath)]) {
      const discovered = await FindEditorInAncestors(start);
      if (discovered) return discovered;
    }
    return null;
  }
}
