// FBZZ GameHub
// templateService.ts | main
// C++版テンプレートのコピーとプレースホルダー展開をTypeScriptで再現する

import { app } from 'electron';
import { access, copyFile, mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { parse } from 'smol-toml';
import { ENGINE_VERSION, type CreateProjectRequest, type TemplateInfo } from '../shared/contracts';

interface ProjectNameInfo {
  name: string;
  projectId: string;
  cppNamespace: string;
  targetName: string;
}

const TEXT_EXTENSIONS = new Set([
  '.cpp', '.hpp', '.h', '.inl', '.txt', '.toml', '.json', '.cmake', '.md', '.hlsl', '.hlsli', '.glsl', '.gitignore',
]);

async function exists(target: string): Promise<boolean> {
  try {
    await access(target);
    return true;
  } catch {
    return false;
  }
}

function makeProjectName(displayName: string): ProjectNameInfo {
  const asciiParts = displayName.match(/[A-Za-z0-9]+/g) ?? [];
  const projectId = asciiParts.join('_').toLowerCase();
  return {
    name: displayName.trim(),
    projectId,
    cppNamespace: projectId,
    targetName: asciiParts.map((part) => `${part[0]?.toUpperCase() ?? ''}${part.slice(1)}`).join(''),
  };
}

function assertValidName(info: ProjectNameInfo): void {
  if (!info.name || !/^[a-z][a-z0-9_]*$/.test(info.projectId) || !info.targetName) {
    throw new Error('プロジェクト名には、英字で始まるASCII英数字を含めてください。');
  }
}

function applyPlaceholders(text: string, info: ProjectNameInfo, createdAt: string, sdkRoot: string): string {
  const replacements: Record<string, string> = {
    PROJECT_NAME: info.name,
    PROJECT_ID: info.projectId,
    CPP_NAMESPACE: info.cppNamespace,
    TARGET_NAME: info.targetName,
    ENGINE_VERSION,
    CREATED_AT: createdAt,
    SETTINGS_PATH: 'ProjectSettings/ProjectSettings.toml',
    API_ROOT: 'Include/',
    PUBLIC_API_HEADER: `Include/${info.targetName}/ProjectAPI.hpp`,
    SCRIPT_ROOT: 'Src/Scripts/',
    LIBRARY_ROOT: 'Lib/',
    BINARY_ROOT: 'Binaries/',
    BUILD_ROOT: 'Build/',
    SDK_ROOT: sdkRoot.replaceAll('\\', '/'),
    ENGINE_ROOT: sdkRoot.replaceAll('\\', '/'),
  };
  return text.replace(/\{\{([A-Z_]+)\}\}/g, (original, key: string) => replacements[key] ?? original);
}

function isTextTemplate(filePath: string): boolean {
  return path.basename(filePath) === 'CMakeLists.txt'
    || filePath.endsWith('.tmpl')
    || TEXT_EXTENSIONS.has(path.extname(filePath).toLowerCase());
}

export class TemplateService {
  private async resolveTemplatesRoot(): Promise<string> {
    const candidates = [
      path.join(process.resourcesPath, 'Templates'),
      path.resolve(app.getAppPath(), '../GameHub/Templates'),
      path.resolve(process.cwd(), 'Projects/GameHub/Templates'),
      path.resolve(process.cwd(), '../GameHub/Templates'),
    ];
    for (const candidate of candidates) {
      if (await exists(candidate)) return candidate;
    }
    throw new Error('GameHubのTemplatesディレクトリが見つかりません。');
  }

  async listTemplates(): Promise<TemplateInfo[]> {
    const root = await this.resolveTemplatesRoot();
    const entries = await readdir(root, { withFileTypes: true });
    const templates = await Promise.all(entries.filter((entry) => entry.isDirectory()).map(async (entry) => {
      try {
        const document = parse(await readFile(path.join(root, entry.name, 'template.toml'), 'utf8')) as Record<string, unknown>;
        const table = (document.template ?? {}) as Record<string, unknown>;
        return {
          id: typeof table.id === 'string' ? table.id : entry.name,
          displayName: typeof table.name === 'string' ? table.name : entry.name,
          description: typeof table.desc === 'string' ? table.desc : '',
        } satisfies TemplateInfo;
      } catch {
        return null;
      }
    }));
    return templates.filter((template): template is TemplateInfo => template !== null);
  }

  async create(request: CreateProjectRequest, sdkRoot: string): Promise<string> {
    const info = makeProjectName(request.displayName);
    assertValidName(info);

    const templatesRoot = await this.resolveTemplatesRoot();
    const templateRoot = path.join(templatesRoot, request.templateId);
    if (!await exists(path.join(templateRoot, 'template.toml'))) {
      throw new Error('選択されたテンプレートが見つかりません。');
    }

    const projectRoot = path.join(path.resolve(request.destinationRoot), info.targetName);
    if (await exists(projectRoot)) throw new Error('同名のプロジェクトフォルダーが既に存在します。');

    const createdAt = new Date().toISOString();
    await mkdir(projectRoot, { recursive: false });
    await this.copyTemplateDirectory(templateRoot, projectRoot, info, createdAt, sdkRoot);

    const sdkAssets = path.join(sdkRoot, 'Assets');
    if (sdkRoot && await exists(sdkAssets)) {
      await this.copyMissingDirectory(sdkAssets, path.join(projectRoot, 'Assets'));
      await this.syncScriptRegistrations(projectRoot, info);
    }
    return projectRoot;
  }

  private async copyTemplateDirectory(sourceRoot: string, projectRoot: string, info: ProjectNameInfo, createdAt: string, sdkRoot: string): Promise<void> {
    const walk = async (sourceDirectory: string, relativeDirectory: string): Promise<void> => {
      for (const entry of await readdir(sourceDirectory, { withFileTypes: true })) {
        if (!relativeDirectory && entry.name === 'template.toml') continue;
        const replacedName = applyPlaceholders(entry.name, info, createdAt, sdkRoot).replace(/\.tmpl$/, '');
        const relativePath = path.join(relativeDirectory, replacedName);
        const sourcePath = path.join(sourceDirectory, entry.name);
        const outputPath = path.join(projectRoot, relativePath);
        if (entry.isDirectory()) {
          await mkdir(outputPath, { recursive: true });
          await walk(sourcePath, relativePath);
        } else if (entry.isFile()) {
          await mkdir(path.dirname(outputPath), { recursive: true });
          if (isTextTemplate(sourcePath)) {
            const text = applyPlaceholders(await readFile(sourcePath, 'utf8'), info, createdAt, sdkRoot);
            await writeFile(outputPath, text, 'utf8');
          } else {
            await copyFile(sourcePath, outputPath);
          }
        }
      }
    };
    await walk(sourceRoot, '');
  }

  private async copyMissingDirectory(sourceRoot: string, destinationRoot: string): Promise<void> {
    await mkdir(destinationRoot, { recursive: true });
    for (const entry of await readdir(sourceRoot, { withFileTypes: true })) {
      const sourcePath = path.join(sourceRoot, entry.name);
      const destinationPath = path.join(destinationRoot, entry.name);
      if (entry.isDirectory()) {
        await this.copyMissingDirectory(sourcePath, destinationPath);
      } else if (entry.isFile() && !await exists(destinationPath)) {
        await copyFile(sourcePath, destinationPath);
      }
    }
  }

  private async syncScriptRegistrations(projectRoot: string, info: ProjectNameInfo): Promise<void> {
    const assetsRoot = path.join(projectRoot, 'Assets');
    const scriptsRoot = path.join(assetsRoot, 'Scripts');
    if (!await exists(scriptsRoot)) return;

    const scripts: Array<{ header: string; entry: string }> = [];
    const visit = async (directory: string): Promise<void> => {
      for (const item of await readdir(directory, { withFileTypes: true })) {
        const itemPath = path.join(directory, item.name);
        if (item.isDirectory()) await visit(itemPath);
        if (!item.isFile() || !item.name.endsWith('.hpp') || item.name.endsWith('.generated.hpp')) continue;
        const source = await readFile(itemPath, 'utf8');
        const namespaceName = source.match(/namespace\s+([A-Za-z_][A-Za-z0-9_:]*)/)?.[1] ?? info.cppNamespace;
        const header = path.relative(assetsRoot, itemPath).replaceAll('\\', '/');
        for (const match of source.matchAll(/FBZZ_SCRIPT\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)/g)) {
          scripts.push({ header, entry: `FBZZ_SCRIPT_ENTRY(${namespaceName}, ${match[1]})` });
        }
      }
    };
    await visit(assetsRoot);
    scripts.sort((left, right) => left.header.localeCompare(right.header) || left.entry.localeCompare(right.entry));

    const includeLines = [...new Set(scripts.map((script) => `#include "${script.header}"`))];
    const entryLines = scripts.map((script) => script.entry);
    await this.replaceMarkerBlock(path.join(projectRoot, 'Src', `${info.targetName}ScriptsDll.cpp`), '@@FBZZ_SCRIPT_INCLUDES_BEGIN', '@@FBZZ_SCRIPT_INCLUDES_END', includeLines);
    await this.replaceMarkerBlock(path.join(projectRoot, 'Src', 'GameMain.cpp'), '@@FBZZ_SCRIPT_INCLUDES_BEGIN', '@@FBZZ_SCRIPT_INCLUDES_END', includeLines);
    await this.replaceMarkerBlock(path.join(scriptsRoot, 'ScriptList.inl'), '@@FBZZ_SCRIPT_ENTRIES_BEGIN', '@@FBZZ_SCRIPT_ENTRIES_END', entryLines);
  }

  private async replaceMarkerBlock(filePath: string, beginMarker: string, endMarker: string, lines: string[]): Promise<void> {
    if (!await exists(filePath)) return;
    const source = await readFile(filePath, 'utf8');
    const begin = `// ${beginMarker}`;
    const end = `// ${endMarker}`;
    const beginIndex = source.indexOf(begin);
    const endIndex = source.indexOf(end, beginIndex + begin.length);
    if (beginIndex < 0 || endIndex < 0) return;
    const lineEnd = source.indexOf('\n', beginIndex) + 1;
    const replacement = `${lines.join('\n')}${lines.length ? '\n' : ''}`;
    await writeFile(filePath, `${source.slice(0, lineEnd)}${replacement}${source.slice(endIndex)}`, 'utf8');
  }
}
