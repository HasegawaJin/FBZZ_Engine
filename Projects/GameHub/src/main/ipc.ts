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

function success<T>(value?: T): OperationResult<T> {
  return { ok: true, value };
}

function failure(error: unknown): OperationResult<never> {
  return { ok: false, error: error instanceof Error ? error.message : String(error) };
}

async function bootstrap(): Promise<BootstrapData> {
  const config = configStore.snapshot();
  return {
    engineVersion: ENGINE_VERSION,
    settings: config.settings,
    projects: await projectService.inspectProjects(config.projects),
    templates: await templateService.listTemplates(),
  };
}

export async function registerIpcHandlers(): Promise<void> {
  await configStore.load();

  ipcMain.handle('hub:bootstrap', async () => {
    try { return success(await bootstrap()); } catch (error) { return failure(error); }
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
      const projectPath = await templateService.create(request, configStore.snapshot().settings.sdkRoot);
      const lastOpened = await configStore.touchProject(projectPath);
      return success(await projectService.inspectProject({ path: projectPath, lastOpened }));
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:remove', async (_event, projectPath: string) => {
    try { await configStore.removeProject(projectPath); return success(); } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:open', async (_event, projectPath: string) => {
    try {
      await projectService.openProject(projectPath, configStore.snapshot().settings);
      await configStore.touchProject(projectPath);
      return success();
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('project:reveal', async (_event, projectPath: string) => {
    try { await projectService.revealProject(projectPath); return success(); } catch (error) { return failure(error); }
  });
  ipcMain.handle('settings:save', async (_event, settings: HubSettings) => {
    try { await configStore.setSettings(settings); return success(await bootstrap()); } catch (error) { return failure(error); }
  });

  ipcMain.on('window:minimize', (event) => BrowserWindow.fromWebContents(event.sender)?.minimize());
  ipcMain.on('window:maximize', (event) => {
    const window = BrowserWindow.fromWebContents(event.sender);
    if (window?.isMaximized()) window.unmaximize(); else window?.maximize();
  });
  ipcMain.on('window:close', (event) => BrowserWindow.fromWebContents(event.sender)?.close());
}
