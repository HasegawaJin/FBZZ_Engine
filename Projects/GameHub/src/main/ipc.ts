// FBZZ GameHub
// ipc.ts | main
// rendererへ公開する操作をホワイトリスト化したIPCハンドラー

import { BrowserWindow, dialog, ipcMain } from 'electron';
import path from 'node:path';
import type { BootstrapData, CreateProjectRequest, HubSettings, OperationResult } from '../shared/contracts';
import { ENGINE_VERSION } from '../shared/contracts';
import { ConfigStore } from './configStore';
import { ProjectService } from './projectService';
import { TemplateService } from './templateService';

const configStore = new ConfigStore();
const projectService = new ProjectService();
const templateService = new TemplateService();
let configLoadPromise: Promise<void> | null = null;

function ensureConfigLoaded(): Promise<void> {
  if (!configLoadPromise) {
    configLoadPromise = configStore.load().then(() => undefined);
  }
  return configLoadPromise;
}

function success<T>(value?: T): OperationResult<T> {
  return { ok: true, value };
}

function failure(error: unknown): OperationResult<never> {
  return { ok: false, error: error instanceof Error ? error.message : String(error) };
}

async function bootstrap(): Promise<BootstrapData> {
  await ensureConfigLoaded();
  const config = configStore.snapshot();
  return {
    engineVersion: ENGINE_VERSION,
    settings: config.settings,
    // 実ファイル検証とサムネイル読込はproject:listへ分離し、初期画面を先に返す。
    projects: projectService.createPendingProjects(config.projects),
    templates: await templateService.listTemplates(),
  };
}

export function registerIpcHandlers(): void {
  // 読込は開始するが、BrowserWindow生成側をブロックしない。
  void ensureConfigLoaded();

  ipcMain.handle('hub:bootstrap', async () => {
    try { return success(await bootstrap()); } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:list', async () => {
    try {
      await ensureConfigLoaded();
      return success(await projectService.inspectProjects(configStore.snapshot().projects));
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('dialog:directory', async () => {
    const result = await dialog.showOpenDialog({ properties: ['openDirectory', 'createDirectory'] });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle('dialog:editor', async () => {
    const result = await dialog.showOpenDialog({ properties: ['openFile'], filters: [{ name: 'FBZZ Editor', extensions: ['exe'] }] });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle('project:add', async () => {
    try {
      await ensureConfigLoaded();
      const result = await dialog.showOpenDialog({ properties: ['openDirectory'] });
      if (result.canceled || !result.filePaths[0]) return failure('プロジェクトの追加をキャンセルしました。');
      const projectPath = path.resolve(result.filePaths[0]);
      const entry = await projectService.inspectProject({ path: projectPath, lastOpened: '' });
      if (!entry.projectFileValid) throw new Error('選択したフォルダーに有効な.fbzz_projがありません。');
      await configStore.addProject(projectPath);
      return success(entry);
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:create', async (_event, request: CreateProjectRequest) => {
    try {
      await ensureConfigLoaded();
      const settings = configStore.snapshot().settings;
      if (!settings.sdkId) throw new Error('新規プロジェクトを作成する前にFBZZ SDKを選択してください。');
      const projectPath = await templateService.create(request, settings.sdkId, ENGINE_VERSION);
      const lastOpened = await configStore.touchProject(projectPath);
      return success(await projectService.inspectProject({ path: projectPath, lastOpened }));
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:remove', async (_event, projectPath: string) => {
    try {
      await ensureConfigLoaded();
      await configStore.removeProject(projectPath);
      return success();
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:open', async (_event, projectPath: string) => {
    try {
      await ensureConfigLoaded();
      const settings = configStore.snapshot().settings;
      const projectSdkId = await projectService.readProjectSdkId(projectPath);
      const sdkRoot = await configStore.resolveSdkRoot(projectSdkId);
      await projectService.openProject(projectPath, settings, sdkRoot);
      await configStore.touchProject(projectPath);
      return success();
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:reveal', async (_event, projectPath: string) => {
    try { await projectService.revealProject(projectPath); return success(); } catch (error) { return failure(error); }
  });
  ipcMain.handle('settings:save', async (_event, settings: HubSettings) => {
    try {
      await ensureConfigLoaded();
      await configStore.setSettings(settings);
      return success(await bootstrap());
    } catch (error) { return failure(error); }
  });

  ipcMain.on('window:minimize', (event) => BrowserWindow.fromWebContents(event.sender)?.minimize());
  ipcMain.on('window:maximize', (event) => {
    const window = BrowserWindow.fromWebContents(event.sender);
    if (window?.isMaximized()) window.unmaximize(); else window?.maximize();
  });
  ipcMain.on('window:close', (event) => BrowserWindow.fromWebContents(event.sender)?.close());
}
