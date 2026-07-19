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

export class ProjectService {
  async inspectProjects(configProjects: ConfigProject[]): Promise<ProjectEntry[]> {
    const projects = await Promise.all(configProjects.map((project) => this.inspectProject(project)));
    return projects.sort((left, right) => right.lastOpened.localeCompare(left.lastOpened));
  }

  async inspectProject(configProject: ConfigProject): Promise<ProjectEntry> {
    const projectRoot = path.resolve(configProject.path);
    const empty: ProjectEntry = {
      name: path.basename(projectRoot), projectId: '', path: projectRoot, engineVersion: '-', lastOpened: configProject.lastOpened,
      thumbnailDataUrl: '', pathExists: false, projectFileValid: false, cmakeExists: false, layoutValid: false,
      generatedRootsExist: false, engineVersionMismatch: false, migrationRequired: false,
    };
    if (!await exists(projectRoot)) return empty;
    empty.pathExists = true;
    empty.cmakeExists = await exists(path.join(projectRoot, 'CMakeLists.txt'));
    empty.layoutValid = (await Promise.all(['Assets', 'Src', 'Include'].map((name) => exists(path.join(projectRoot, name))))).every(Boolean);
    empty.generatedRootsExist = (await Promise.all(['Lib', 'Binaries', 'Build'].map((name) => exists(path.join(projectRoot, name))))).every(Boolean);
    empty.thumbnailDataUrl = await this.loadThumbnail(projectRoot);

    try {
      const document = parse(await readFile(path.join(projectRoot, '.fbzz_proj'), 'utf8')) as Record<string, unknown>;
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
    const child = spawn(editorPath, ['--project', path.resolve(projectPath)], {
      detached: true,
      stdio: 'ignore',
      windowsHide: false,
    });
    child.unref();
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
      path.resolve(process.cwd(), 'build/Debug/Binaries/Debug/Editor/FBZZEditor.exe'),
      path.resolve(process.cwd(), 'build/Development/Binaries/Development/Editor/FBZZEditor.exe'),
      path.resolve(process.cwd(), 'build/Release/Binaries/Release/Editor/FBZZEditor.exe'),
      path.resolve(app.getAppPath(), '../../build/Debug/Binaries/Debug/Editor/FBZZEditor.exe'),
    ].filter(Boolean);
    for (const candidate of candidates) {
      if (await exists(candidate)) return candidate;
    }
    return null;
  }
}
