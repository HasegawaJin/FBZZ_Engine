// FBZZ GameHub
// global.d.ts | renderer
// contextBridge が公開するAPIをrendererで型安全に利用するための宣言

import type { GameHubApi } from './shared/contracts';

declare global {
  interface Window {
    gameHub: GameHubApi;
  }
}

export {};
