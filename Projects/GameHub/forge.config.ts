/// @file    forge.config.ts
/// @brief   Forge の起動、パッケージ生成と Electron Fuse の設定。
/// @author  Hasegawa Jin
/// @date    2026/07/19

import type { ForgeConfig } from '@electron-forge/shared-types';
import { MakerSquirrel } from '@electron-forge/maker-squirrel';
import { MakerZIP } from '@electron-forge/maker-zip';
import { MakerDeb } from '@electron-forge/maker-deb';
import { MakerRpm } from '@electron-forge/maker-rpm';
import { VitePlugin } from '@electron-forge/plugin-vite';
import { FusesPlugin } from '@electron-forge/plugin-fuses';
import { FuseV1Options, FuseVersion } from '@electron/fuses';
import path from 'node:path';
import { GAME_HUB_BUILD_CONFIGURATION } from './build.config';

const config: ForgeConfig = {
  outDir: 'out',
  buildIdentifier: GAME_HUB_BUILD_CONFIGURATION,
  packagerConfig: {
    asar: true,
    name: 'FBZZGameHub',
    executableName: 'FBZZGameHub',
    icon: process.platform === 'win32' ? path.resolve(__dirname, 'Branding', 'GameHub.ico') : undefined,
    extraResource: [path.resolve(__dirname, 'Templates')],
  },
  rebuildConfig: {},
  makers: [
    new MakerSquirrel({ name: 'FBZZGameHub' }),
    new MakerZIP({}, ['darwin']),
    new MakerRpm({}),
    new MakerDeb({}),
  ],
  plugins: [
    new VitePlugin({
      build: [
        {
          entry: 'src/main.ts',
          config: 'vite.main.config.ts',
          target: 'main',
        },
        {
          entry: 'src/preload.ts',
          config: 'vite.preload.config.ts',
          target: 'preload',
        },
      ],
      renderer: [
        {
          name: 'main_window',
          config: 'vite.renderer.config.ts',
        },
      ],
    }),
    new FusesPlugin({
      version: FuseVersion.V1,
      [FuseV1Options.RunAsNode]: false,
      [FuseV1Options.EnableCookieEncryption]: true,
      [FuseV1Options.EnableNodeOptionsEnvironmentVariable]: false,
      /// @note VS Code からの main プロセス調査は Debug と Development で許可する。
      [FuseV1Options.EnableNodeCliInspectArguments]: GAME_HUB_BUILD_CONFIGURATION !== 'Release',
      [FuseV1Options.EnableEmbeddedAsarIntegrityValidation]: true,
      [FuseV1Options.OnlyLoadAppFromAsar]: true,
    }),
  ],
};

export default config;
