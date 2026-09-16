// FBZZ GameHub
// preload.ts | preload
// Node.jsをrendererへ露出せず、用途を限定した型付きAPIだけを橋渡しする

import { contextBridge, ipcRenderer } from 'electron';
import type { CreateProjectRequest, GameHubApi, HubSettings } from './shared/contracts';

const api: GameHubApi = {
  bootstrap: () => ipcRenderer.invoke('hub:bootstrap'),
  listProjects: () => ipcRenderer.invoke('project:list'),
  chooseDirectory: () => ipcRenderer.invoke('dialog:directory'),
  chooseEditorExecutable: () => ipcRenderer.invoke('dialog:editor'),
  addProject: () => ipcRenderer.invoke('project:add'),
  createProject: (request: CreateProjectRequest) => ipcRenderer.invoke('project:create', request),
  removeProject: (projectPath: string) => ipcRenderer.invoke('project:remove', projectPath),
  openProject: (projectPath: string) => ipcRenderer.invoke('project:open', projectPath),
  revealProject: (projectPath: string) => ipcRenderer.invoke('project:reveal', projectPath),
  saveSettings: (settings: HubSettings) => ipcRenderer.invoke('settings:save', settings),
  onSettingsUpdated: (callback) => {
    const listener = (_event: Electron.IpcRendererEvent, settings: HubSettings) => callback(settings);
    ipcRenderer.on('hub:settings-updated', listener);
    return () => ipcRenderer.removeListener('hub:settings-updated', listener);
  },
  minimizeWindow: () => ipcRenderer.send('window:minimize'),
  maximizeWindow: () => ipcRenderer.send('window:maximize'),
  closeWindow: () => ipcRenderer.send('window:close'),
};

contextBridge.exposeInMainWorld('gameHub', api);
