/// @file ipc.ts
/// @brief renderer へ公開する操作をホワイトリスト化した IPC ハンドラー。
/// @author Hasegawa Jin
/// @date 2026/07/19

import { BrowserWindow, dialog, ipcMain, shell } from 'electron';
import path from 'node:path';
import type { BootstrapData, CreateProjectRequest, HubSettings, OperationResult, UpdateNotice } from '../shared/contracts';
import { ENGINE_VERSION } from '../shared/contracts';
import { outdatedSdkNotice } from '../shared/SdkNotice';
import { ConfigStore } from './configStore';
import { ProjectService } from './projectService';
import { TemplateService } from './templateService';
import { fetchLatestRelease, isFresh, isReleasePageUrl, noticeFrom } from './updateChecker';

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

function cancelled(): OperationResult<never> {
  return { ok: true, cancelled: true };
}

function failure(error: unknown): OperationResult<never> {
  return { ok: false, error: error instanceof Error ? error.message : String(error) };
}

/// @note 保存後も通知し、renderer が保存値を起動時の検出結果で上書きするのを防ぐ。
function broadcastSettings(settings: HubSettings): void {
  for (const window of BrowserWindow.getAllWindows()) {
    window.webContents.send('hub:settings-updated', settings);
  }
}

function publishResolvedSettings(): void {
  void ensureConfigLoaded()
    .then(() => configStore.ensureSdkResolved())
    .then(() => broadcastSettings(configStore.snapshot().settings));
}

async function bootstrap(): Promise<BootstrapData> {
  await ensureConfigLoaded();
  const config = configStore.snapshot();
  return {
    engineVersion: ENGINE_VERSION,
    settings: config.settings,
    /// @note 実ファイル検証とサムネイル読込はproject:listへ分離し、初期画面を先に返す。
    projects: projectService.createPendingProjects(config.projects),
    templates: await templateService.listTemplates(),
  };
}

/// @note 知らせるべき新しい版を返す。24 時間以内に確認済みなら問い合わせず保存済みの結果を使う。
/// @see Docs/design/gamehub-update-notice.md
async function checkForUpdate(): Promise<UpdateNotice | null> {
  await ensureConfigLoaded();
  const config = configStore.snapshot();
  if (!config.settings.checkForUpdates) return null;

  let state = config.update;
  if (!isFresh(state, Date.now())) {
    const latest = await fetchLatestRelease();
    if (latest) {
      state = { ...state, ...latest, lastCheckedAt: new Date().toISOString() };
      await configStore.setUpdateState(state);
    }
  }
  return noticeFrom(state, ENGINE_VERSION);
}

export function registerIpcHandlers(): void {
  /// @note 読込は開始するが、BrowserWindow生成側をブロックしない。
  void ensureConfigLoaded();
  publishResolvedSettings();

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
  ipcMain.handle('project:add', async () => {
    try {
      await ensureConfigLoaded();
      const result = await dialog.showOpenDialog({ properties: ['openDirectory'] });
      if (result.canceled || !result.filePaths[0]) return cancelled();
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
      await configStore.ensureSdkResolved();
      const settings = configStore.snapshot().settings;
      if (!settings.sdkId) throw new Error('新規プロジェクトを作成する前にFBZZ SDKを選択してください。');
      const projectPath = await templateService.create(request, settings.sdkId);
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
      await configStore.ensureSdkResolved();
      const settings = configStore.snapshot().settings;
      const projectSdkId = await projectService.readProjectSdkId(projectPath);
      const sdkRoot = await configStore.resolveSdkRoot(projectSdkId);
      const sdkNotice = outdatedSdkNotice(projectSdkId, settings.sdkId, ENGINE_VERSION);
      if (sdkRoot && sdkNotice) {
        const result = await dialog.showMessageBox({
          type: 'warning',
          title: '古い SDK で起動します',
          message: sdkNotice,
          detail: `プロジェクト: ${projectPath}\nSDK: ${sdkRoot}\n構成: ${settings.sdkConfiguration}`,
          buttons: ['キャンセル', `SDK ${projectSdkId || settings.sdkId} で開く`],
          defaultId: 0,
          cancelId: 0,
        });
        if (result.response !== 1) return cancelled();
      }
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
      broadcastSettings(configStore.snapshot().settings);
      return success(await bootstrap());
    } catch (error) { return failure(error); }
  });

  ipcMain.handle('update:check', async () => {
    try { return success(await checkForUpdate()); } catch (error) { return failure(error); }
  });
  ipcMain.handle('update:dismiss', async (_event, version: string) => {
    try {
      await ensureConfigLoaded();
      await configStore.setUpdateState({ ...configStore.snapshot().update, dismissedVersion: String(version) });
      return success();
    } catch (error) { return failure(error); }
  });
  ipcMain.handle('update:open-release', async () => {
    try {
      await ensureConfigLoaded();
      const url = configStore.snapshot().update.latestUrl;
      /// @note renderer から URL を受け取らない。保存済みの値もこのリポジトリのリリースページでなければ開かない。
      if (!isReleasePageUrl(url)) throw new Error('リリースページの URL が不正です。');
      await shell.openExternal(url);
      return success();
    } catch (error) { return failure(error); }
  });

  ipcMain.on('window:minimize', (event) => BrowserWindow.fromWebContents(event.sender)?.minimize());
  ipcMain.on('window:maximize', (event) => {
    const window = BrowserWindow.fromWebContents(event.sender);
    if (window?.isMaximized()) window.unmaximize(); else window?.maximize();
  });
  ipcMain.on('window:close', (event) => BrowserWindow.fromWebContents(event.sender)?.close());
}
