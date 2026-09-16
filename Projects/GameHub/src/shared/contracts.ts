/**
 * @file contracts.ts
 * @brief main / preload / renderer 間で共有する型付き契約。
 * @author Hasegawa Jin
 * @date 2026/09/02
 */

export const ENGINE_VERSION = '0.1.0';

export type HubTheme = 'modern' | 'dark' | 'light' | 'system';
export type SdkBuildConfiguration = 'Debug' | 'Development' | 'Release';

export interface HubSettings {
  editorExe: string;
  sdkRoot: string;
  sdkId: string;
  sdkConfiguration: SdkBuildConfiguration;
  theme: HubTheme;
}

export interface ProjectEntry {
  name: string;
  projectId: string;
  path: string;
  engineVersion: string;
  sdkId: string;
  lastOpened: string;
  thumbnailDataUrl: string;
  pathExists: boolean;
  projectFileValid: boolean;
  cmakeExists: boolean;
  layoutValid: boolean;
  generatedRootsExist: boolean;
  engineVersionMismatch: boolean;
  migrationRequired: boolean;
  // true の間はファイルシステム検証をバックグラウンドで実行中。
  validationPending: boolean;
}

export interface TemplateInfo {
  id: string;
  displayName: string;
  description: string;
}

export interface BootstrapData {
  engineVersion: string;
  projects: ProjectEntry[];
  settings: HubSettings;
  templates: TemplateInfo[];
}

export interface CreateProjectRequest {
  displayName: string;
  destinationRoot: string;
  templateId: string;
}

export interface ProjectIdentifiers {
  name: string;
  projectId: string;
  cppNamespace: string;
  targetName: string;
}

/**
 * 表示名から、フォルダー名・識別子・C++ 名前空間を導出する。
 * WHY: renderer は作成前に生成先パスを提示し、main は同じ規則でフォルダーを作る。
 *      規則が二重定義になるとプレビューと実際の作成結果がずれるため共有する。
 */
export function deriveProjectIdentifiers(displayName: string): ProjectIdentifiers {
  const asciiParts = displayName.match(/[A-Za-z0-9]+/g) ?? [];
  const projectId = asciiParts.join('_').toLowerCase();
  return {
    name: displayName.trim(),
    projectId,
    cppNamespace: projectId,
    targetName: asciiParts.map((part) => `${part[0]?.toUpperCase() ?? ''}${part.slice(1)}`).join(''),
  };
}

/** 導出結果がプロジェクト名・CMake ターゲット名として使えるか。 */
export function isValidProjectIdentifiers(identifiers: ProjectIdentifiers): boolean {
  return Boolean(identifiers.name) && /^[a-z][a-z0-9_]*$/.test(identifiers.projectId) && Boolean(identifiers.targetName);
}

export interface OperationResult<T = undefined> {
  ok: boolean;
  value?: T;
  error?: string;
}

export interface GameHubApi {
  bootstrap(): Promise<OperationResult<BootstrapData>>;
  listProjects(): Promise<OperationResult<ProjectEntry[]>>;
  chooseDirectory(): Promise<string | null>;
  chooseEditorExecutable(): Promise<string | null>;
  addProject(): Promise<OperationResult<ProjectEntry>>;
  createProject(request: CreateProjectRequest): Promise<OperationResult<ProjectEntry>>;
  removeProject(projectPath: string): Promise<OperationResult>;
  openProject(projectPath: string): Promise<OperationResult>;
  revealProject(projectPath: string): Promise<OperationResult>;
  saveSettings(settings: HubSettings): Promise<OperationResult<BootstrapData>>;
  onSettingsUpdated(callback: (settings: HubSettings) => void): () => void;
  minimizeWindow(): void;
  maximizeWindow(): void;
  closeWindow(): void;
}
