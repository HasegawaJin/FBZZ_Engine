/// @file contracts.ts
/// @brief main / preload / renderer 間で共有する型付き契約。
/// @author Hasegawa Jin
/// @date 2026/09/02

/// @note build.config.ts が正本の Engine 版と配布構成を注入する。
declare const __FBZZ_ENGINE_VERSION__: string;
declare const __FBZZ_BUILD_CONFIGURATION__: SdkBuildConfiguration;
export const ENGINE_VERSION: string = __FBZZ_ENGINE_VERSION__;
export const GAME_HUB_BUILD_CONFIGURATION: SdkBuildConfiguration = __FBZZ_BUILD_CONFIGURATION__;

/// @note 先頭の v と世代 ID の接尾辞を除いて版を比較する。
/// @return 正なら left が新しい、0 なら同じ。解釈不能なら null。
export function compareVersions(left: string, right: string): number | null {
  const parseVersion = (value: string) => value.trim().replace(/^v/i, '').split('-')[0]?.split('.').map(Number) ?? [];
  const leftParts = parseVersion(left);
  const rightParts = parseVersion(right);
  if (leftParts.length === 0 || rightParts.length === 0) return null;
  if ([...leftParts, ...rightParts].some(Number.isNaN)) return null;
  for (let index = 0; index < Math.max(leftParts.length, rightParts.length); index += 1) {
    const difference = (leftParts[index] ?? 0) - (rightParts[index] ?? 0);
    if (difference !== 0) return difference;
  }
  return 0;
}

/// @return minimum が空なら true。解釈不能な版は互換と見なさない。
export function isVersionAtLeast(current: string, minimum: string): boolean {
  if (!minimum.trim()) return true;
  const difference = compareVersions(current, minimum);
  return difference !== null && difference >= 0;
}

export type HubTheme = 'modern' | 'dark' | 'light' | 'system';
export type SdkBuildConfiguration = 'Debug' | 'Development' | 'Release';

/// @brief 選択 SDK と構成だけから Editor の配置を求める。
/// @note renderer と main で同じ配置を使い、手動 Editor 指定を起動先へ混ぜない。
export function GetSdkEditorPath(sdkRoot: string, configuration: SdkBuildConfiguration): string {
  return sdkRoot.trim() ? `${sdkRoot.trim().replace(/[\\/]+$/, '')}/tools/${configuration}/Editor/FBZZEditor.exe` : '';
}

export interface HubSettings {
  /// @note 旧設定の保存互換のみ。起動先は SDK と構成から求める。
  editorExe: string;
  sdkRoot: string;
  sdkId: string;
  sdkConfiguration: SdkBuildConfiguration;
  theme: HubTheme;
  /// @see Docs/design/gamehub-update-notice.md
  checkForUpdates: boolean;
}

/// @note 新しい版の判定は main で行う。
export interface UpdateNotice {
  currentVersion: string;
  latestVersion: string;
  /// @note リリースノートの冒頭だけをプレーンテキストで渡す。
  notes: string;
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
  /// @note true の間は実ファイルの検証が未完了。
  validationPending: boolean;
}

export interface TemplateInfo {
  id: string;
  displayName: string;
  description: string;
  /// @note template.toml の engine_version_min。空なら制約なし。
  engineVersionMin: string;
  /// @note main が Engine の版と照合済み。
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

/// @note 作成プレビューと実際の識別子生成は同じ規則を使う。
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

export function isValidProjectIdentifiers(identifiers: ProjectIdentifiers): boolean {
  return Boolean(identifiers.name) && /^[a-z][a-z0-9_]*$/.test(identifiers.projectId) && Boolean(identifiers.targetName);
}

export interface OperationResult<T = undefined> {
  ok: boolean;
  value?: T;
  error?: string;
  /// @note ダイアログの取りやめは失敗の通知と再読込を行わない。
  cancelled?: boolean;
}

export interface GameHubApi {
  bootstrap(): Promise<OperationResult<BootstrapData>>;
  listProjects(): Promise<OperationResult<ProjectEntry[]>>;
  chooseDirectory(): Promise<string | null>;
  addProject(): Promise<OperationResult<ProjectEntry>>;
  createProject(request: CreateProjectRequest): Promise<OperationResult<ProjectEntry>>;
  removeProject(projectPath: string): Promise<OperationResult>;
  openProject(projectPath: string): Promise<OperationResult>;
  revealProject(projectPath: string): Promise<OperationResult>;
  saveSettings(settings: HubSettings): Promise<OperationResult<BootstrapData>>;
  onSettingsUpdated(callback: (settings: HubSettings) => void): () => void;
  /// @return 新しい版を知らせる必要がなければ value は null。
  checkForUpdate(): Promise<OperationResult<UpdateNotice | null>>;
  dismissUpdate(version: string): Promise<OperationResult>;
  /// @note URL は main が保存した値だけを使用する。
  openReleasePage(): Promise<OperationResult>;
  minimizeWindow(): void;
  maximizeWindow(): void;
  closeWindow(): void;
}
