/// @file    templateService.ts
/// @brief   テンプレートと共通 C# starter から新規プロジェクトを生成する。
/// @author  Hasegawa Jin
/// @date    2026-07-19

import { app } from 'electron';
import { access, copyFile, mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { parse } from 'smol-toml';
import type { CreateProjectRequest, ProjectIdentifiers, TemplateInfo } from '../shared/contracts';
import { ENGINE_VERSION, deriveProjectIdentifiers, isValidProjectIdentifiers, isVersionAtLeast } from '../shared/contracts';
import type { SdkBuildConfiguration } from '../shared/contracts';
import { PrepareManagedStarter, WriteManagedStarter } from './managedStarter';

const TEXT_EXTENSIONS = new Set([
  '.cpp', '.hpp', '.h', '.inl', '.txt', '.toml', '.json', '.cmake', '.md', '.hlsl', '.hlsli', '.glsl', '.gitignore',
]);

/// @note ローカルのビルド出力と廃止済みの C++ Script 用ヘッダーを新規プロジェクトへ持ち込まない。
const EXCLUDED_ROOT_DIRECTORIES = new Set(['Binaries', 'Build', 'Include', 'Lib', 'Library']);

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

function applyPlaceholders(text: string, info: ProjectIdentifiers, createdAt: string, sdkId: string): string {
  const replacements: Record<string, string> = {
    PROJECT_NAME: info.name,
    PROJECT_ID: info.projectId,
    TARGET_NAME: info.targetName,
    ENGINE_VERSION,
    SDK_ID: sdkId,
    CREATED_AT: createdAt,
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
    /// @note bootstrap の反復で同じ template.toml を再走査しない。
    if (!this.templatesPromise) {
      this.templatesPromise = this.loadTemplates().catch((error: unknown) => {
        /// @note 一時的なアクセス失敗は次の要求で再試行する。
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
        const engineVersionMin = typeof table.engine_version_min === 'string' ? table.engine_version_min : '';
        return {
          id: typeof table.id === 'string' ? table.id : entry.name,
          displayName: typeof table.display_name === 'string' ? table.display_name : entry.name,
          description: typeof table.description === 'string' ? table.description : '',
          engineVersionMin,
          compatible: isVersionAtLeast(ENGINE_VERSION, engineVersionMin),
        } satisfies TemplateInfo;
      } catch {
        return null;
      }
    }));
    return templates.filter((template): template is TemplateInfo => template !== null);
  }

  async create(request: CreateProjectRequest, sdkId: string, sdkRoot = '', sdkConfiguration: SdkBuildConfiguration = 'Development'): Promise<string> {
    const info = deriveProjectIdentifiers(request.displayName);
    assertValidName(info);
    const managedStarter = await PrepareManagedStarter(sdkRoot, sdkConfiguration);

    const template = (await this.listTemplates()).find((candidate) => candidate.id === request.templateId);
    const templatesRoot = await this.resolveTemplatesRoot();
    const templateRoot = path.join(templatesRoot, request.templateId);
    if (!template || !await exists(path.join(templateRoot, 'template.toml'))) {
      throw new Error('選択されたテンプレートが見つかりません。');
    }
    if (!template.compatible) {
      throw new Error(`テンプレート ${template.displayName} は Engine ${template.engineVersionMin} 以上が必要です (GameHub は ${ENGINE_VERSION})。`);
    }

    const projectRoot = path.join(path.resolve(request.destinationRoot), info.targetName);
    if (await exists(projectRoot)) throw new Error('同名のプロジェクトフォルダーが既に存在します。');

    const createdAt = new Date().toISOString();
    await mkdir(projectRoot, { recursive: false });
    await this.copyTemplateDirectory(templateRoot, projectRoot, info, createdAt, sdkId);
    await WriteManagedStarter(projectRoot, info.targetName, sdkRoot, sdkConfiguration, managedStarter);
    return projectRoot;
  }

  private async copyTemplateDirectory(sourceRoot: string, projectRoot: string, info: ProjectIdentifiers, createdAt: string, sdkId: string): Promise<void> {
    const walk = async (sourceDirectory: string, relativeDirectory: string): Promise<void> => {
      for (const entry of await readdir(sourceDirectory, { withFileTypes: true })) {
        if (!relativeDirectory && entry.name === 'template.toml') continue;
        if (!relativeDirectory && entry.isDirectory() && EXCLUDED_ROOT_DIRECTORIES.has(entry.name)) continue;
        if (relativeDirectory === 'Src' && entry.isDirectory() && entry.name === 'Scripts') continue;
        const replacedName = applyPlaceholders(entry.name, info, createdAt, sdkId).replace(/\.tmpl$/, '');
        const relativePath = path.join(relativeDirectory, replacedName);
        const sourcePath = path.join(sourceDirectory, entry.name);
        const outputPath = path.join(projectRoot, relativePath);
        if (entry.isDirectory()) {
          await mkdir(outputPath, { recursive: true });
          await walk(sourcePath, relativePath);
        } else if (entry.isFile()) {
          await mkdir(path.dirname(outputPath), { recursive: true });
          if (isTextTemplate(sourcePath)) {
            const text = applyPlaceholders(await readFile(sourcePath, 'utf8'), info, createdAt, sdkId);
            await writeFile(outputPath, text, 'utf8');
          } else {
            await copyFile(sourcePath, outputPath);
          }
        }
      }
    };
    await walk(sourceRoot, '');
  }

}
