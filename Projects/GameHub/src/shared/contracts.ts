/**
 * @file contracts.ts
 * @brief main / preload / renderer 間で共有する型付き契約。
 * @author Hasegawa Jin
 * @date 2026/09/02
 */

/** ビルド時に Vite の define が注入する。正本はリポジトリ直下の CMakeLists.txt (build.config.ts 参照)。 */
declare const __FBZZ_ENGINE_VERSION__: string;
export const ENGINE_VERSION: string = __FBZZ_ENGINE_VERSION__;

/**
 * "a.b.c" 形式の version 比較。minimum が空なら制約なし。
 * どちらかが解釈できなければ false (互換を推測しない)。
 */
export function isVersionAtLeast(current: string, minimum: string): boolean {
  if (!minimum.trim()) return true;
  const parseVersion = (value: string) => value.trim().split('-')[0]?.split('.').map(Number) ?? [];
  const currentParts = parseVersion(current);
  const minimumParts = parseVersion(minimum);
  if (currentParts.length === 0 || minimumParts.length === 0) return false;
  if ([...currentParts, ...minimumParts].some(Number.isNaN)) return false;
  for (let index = 0; index < Math.max(currentParts.length, minimumParts.length); index += 1) {
    const left = currentParts[index] ?? 0;
    const right = minimumParts[index] ?? 0;
    if (left !== right) return left > right;
  }
  return true;
}

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
  // template.toml の engine_version_min。空なら制約なし。
  engineVersionMin: string;
  // この GameHub の Engine version で作成できるか (main が判定し、renderer は表示だけ)。
  compatible: boolean;
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
  // ユーザーがダイアログで取りやめた。失敗ではないので通知も再読込もしない。
  cancelled?: boolean;
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
