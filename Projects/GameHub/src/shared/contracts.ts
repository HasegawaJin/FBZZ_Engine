// FBZZ GameHub
// contracts.ts | shared
// main / preload / renderer 間で共有する型付き契約

export const ENGINE_VERSION = '0.1.0';

export type HubTheme = 'midnight' | 'dark';
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
  minimizeWindow(): void;
  maximizeWindow(): void;
  closeWindow(): void;
}
