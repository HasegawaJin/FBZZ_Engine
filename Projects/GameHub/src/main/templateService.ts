// FBZZ GameHub
// templateService.ts | main
// C++版テンプレートのコピーとプレースホルダー展開をTypeScriptで再現する

import { app } from 'electron';
import { access, copyFile, mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { parse } from 'smol-toml';
import type { CreateProjectRequest, ProjectIdentifiers, TemplateInfo } from '../shared/contracts';
import { deriveProjectIdentifiers, isValidProjectIdentifiers } from '../shared/contracts';

const TEXT_EXTENSIONS = new Set([
  '.cpp', '.hpp', '.h', '.inl', '.txt', '.toml', '.json', '.cmake', '.md', '.hlsl', '.hlsli', '.glsl', '.gitignore',
]);

// ビルド生成物はテンプレート作業中に混入しても新規プロジェクトへ持ち込まない。
// WHY: CMakeCache.txt には生成元の絶対パスが焼き込まれ、コピー先の configure を必ず失敗させる。
const GENERATED_ROOT_DIRECTORIES = new Set(['Binaries', 'Build', 'Lib', 'Library']);

async function exists(target: string): Promise<boolean> {
  try {
    await access(target);
    return true;
  } catch {
    return false;
  }
}

function assertValidName(info: ProjectIdentifiers): void {
  if (!isValidProjectIdentifiers(info)) {
    throw new Error('プロジェクト名には、英字で始まるASCII英数字を含めてください。');
  }
}

function applyPlaceholders(text: string, info: ProjectIdentifiers, createdAt: string, sdkId: string, engineVersion: string): string {
  const replacements: Record<string, string> = {
    PROJECT_NAME: info.name,
    PROJECT_ID: info.projectId,
    CPP_NAMESPACE: info.cppNamespace,
    TARGET_NAME: info.targetName,
    ENGINE_VERSION: engineVersion,
    SDK_ID: sdkId,
    CREATED_AT: createdAt,
    SETTINGS_PATH: 'ProjectSettings/ProjectSettings.toml',
    API_ROOT: 'Include/',
    PUBLIC_API_HEADER: `Include/${info.targetName}/ProjectAPI.hpp`,
    SCRIPT_ROOT: 'Src/Scripts/',
    LIBRARY_ROOT: 'Lib/',
    BINARY_ROOT: 'Binaries/',
    BUILD_ROOT: 'Build/',
  };
  return text.replace(/\{\{([A-Z_]+)\}\}/g, (original, key: string) => replacements[key] ?? original);
}

function isTextTemplate(filePath: string): boolean {
  return path.basename(filePath) === 'CMakeLists.txt'
    || path.basename(filePath) === '.fbzz_proj'
    || filePath.endsWith('.tmpl')
    || TEXT_EXTENSIONS.has(path.extname(filePath).toLowerCase());
}

export class TemplateService {
  private templatesPromise: Promise<TemplateInfo[]> | null = null;

  private async resolveTemplatesRoot(): Promise<string> {
    const candidates = [
      path.join(process.resourcesPath, 'Templates'),
      path.resolve(app.getAppPath(), 'Templates'),
      path.resolve(process.cwd(), 'Templates'),
      path.resolve(process.cwd(), 'Projects/GameHub/Templates'),
    ];
    for (const candidate of candidates) {
      if (await exists(candidate)) return candidate;
    }
    throw new Error('GameHubのTemplatesディレクトリが見つかりません。');
  }

  async listTemplates(): Promise<TemplateInfo[]> {
    // テンプレートはGameHubの実行中に変化しないため、一覧をプロセス内で共有する。
    // WHY: bootstrap は設定保存後や操作完了後にも呼ばれる。毎回 template.toml を
    //      探して読むと、起動直後のStrictMode再実行も含めて不要なI/Oが増える。
    if (!this.templatesPromise) {
      this.templatesPromise = this.loadTemplates().catch((error: unknown) => {
        // 一時的なファイルアクセス失敗から復帰できるよう、失敗したPromiseは捨てる。
        this.templatesPromise = null;
        throw error;
      });
    }
    return this.templatesPromise;
  }

  private async loadTemplates(): Promise<TemplateInfo[]> {
    const root = await this.resolveTemplatesRoot();
    const entries = await readdir(root, { withFileTypes: true });
    const templates = await Promise.all(entries.filter((entry) => entry.isDirectory()).map(async (entry) => {
      try {
        const document = parse(await readFile(path.join(root, entry.name, 'template.toml'), 'utf8')) as Record<string, unknown>;
        const table = (document.template ?? {}) as Record<string, unknown>;
        return {
          id: typeof table.id === 'string' ? table.id : entry.name,
          displayName: typeof table.display_name === 'string' ? table.display_name : entry.name,
          description: typeof table.description === 'string' ? table.description : '',
        } satisfies TemplateInfo;
      } catch {
        return null;
      }
    }));
    return templates.filter((template): template is TemplateInfo => template !== null);
  }

  async create(request: CreateProjectRequest, sdkId: string, engineVersion: string): Promise<string> {
    const info = deriveProjectIdentifiers(request.displayName);
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
    await this.copyTemplateDirectory(templateRoot, projectRoot, info, createdAt, sdkId, engineVersion);
    await this.syncScriptRegistrations(projectRoot, info);
    return projectRoot;
  }

  private async copyTemplateDirectory(sourceRoot: string, projectRoot: string, info: ProjectIdentifiers, createdAt: string, sdkId: string, engineVersion: string): Promise<void> {
    const walk = async (sourceDirectory: string, relativeDirectory: string): Promise<void> => {
      for (const entry of await readdir(sourceDirectory, { withFileTypes: true })) {
        if (!relativeDirectory && entry.name === 'template.toml') continue;
        const replacedName = applyPlaceholders(entry.name, info, createdAt, sdkId, engineVersion).replace(/\.tmpl$/, '');
        const relativePath = path.join(relativeDirectory, replacedName);
        const sourcePath = path.join(sourceDirectory, entry.name);
        const outputPath = path.join(projectRoot, relativePath);
        const normalizedRelativePath = relativePath.replaceAll('\\', '/');
        if (normalizedRelativePath === 'Assets/Shaders' || normalizedRelativePath === 'Assets/Shaders.meta') continue;
        if (!relativeDirectory && entry.isDirectory() && GENERATED_ROOT_DIRECTORIES.has(entry.name)) continue;
        if (entry.isDirectory()) {
          // Engine shaderはSDKのread-only共有assetを使用し、プロジェクトへ複製しない。
          await mkdir(outputPath, { recursive: true });
          await walk(sourcePath, relativePath);
        } else if (entry.isFile()) {
          await mkdir(path.dirname(outputPath), { recursive: true });
          if (isTextTemplate(sourcePath)) {
            const text = applyPlaceholders(await readFile(sourcePath, 'utf8'), info, createdAt, sdkId, engineVersion);
            await writeFile(outputPath, text, 'utf8');
          } else {
            await copyFile(sourcePath, outputPath);
          }
        }
      }
    };
    await walk(sourceRoot, '');
  }

  private async syncScriptRegistrations(projectRoot: string, info: ProjectIdentifiers): Promise<void> {
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
