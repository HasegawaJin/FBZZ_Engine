/**
 * @file global.d.ts
 * @brief contextBridge が公開する API を renderer で型安全に利用するための宣言。
 * @author Hasegawa Jin
 * @date 2026/07/19
 */

import type { GameHubApi } from './shared/contracts';

declare global {
  interface Window {
    gameHub: GameHubApi;
  }
}

export {};
