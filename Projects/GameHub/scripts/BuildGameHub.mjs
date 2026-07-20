// FBZZ Engine
// BuildGameHub.mjs | GameHub
// Vite生成物とElectronランタイムを構成別の実行可能パッケージへ組み立てる

import { cp, mkdir, readFile, rename, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { build as BuildViteBundle } from 'vite';

const require = createRequire(import.meta.url);
const ViteConfigGenerator = require('@electron-forge/plugin-vite/dist/ViteConfig.js').default;
const { createPackage } = require('@electron/asar');
const { flipFuses, FuseV1Options, FuseVersion } = require('@electron/fuses');
const VALID_CONFIGURATIONS = new Set(['Debug', 'Development', 'Release']);

/** Forge Vite Pluginと同じ基底設定を生成し、main・preload・rendererを本番形式でビルドする。 */
async function BuildVite(projectRoot) {
  const pluginConfig = {
    build: [
      { entry: 'src/main.ts', config: 'vite.main.config.ts', target: 'main' },
      { entry: 'src/preload.ts', config: 'vite.preload.config.ts', target: 'preload' },
    ],
    renderer: [
      { name: 'main_window', config: 'vite.renderer.config.ts' },
    ],
  };
  const generator = new ViteConfigGenerator(pluginConfig, projectRoot, true);

  await rm(path.join(projectRoot, '.vite'), { recursive: true, force: true });
  const buildConfigs = await generator.getBuildConfigs();
  await Promise.all(buildConfigs.map((config) => BuildViteBundle(config)));
  const rendererConfigs = await generator.getRendererConfig();
  await Promise.all(rendererConfigs.map((config) => BuildViteBundle(config)));
}

/** Electron本体、アプリ、Templatesを配置し、配布構成に応じてASARとFuseを適用する。 */
async function AssemblePackage(projectRoot, configuration) {
  const configurationRoot = path.join(projectRoot, 'out', configuration);
  const packageRoot = path.join(configurationRoot, 'FBZZGameHub-win32-x64');
  const stagingRoot = path.join(configurationRoot, '.app-staging');
  const resourcesRoot = path.join(packageRoot, 'resources');
  const executablePath = path.join(packageRoot, 'FBZZGameHub.exe');

  await rm(configurationRoot, { recursive: true, force: true });
  await mkdir(configurationRoot, { recursive: true });
  await cp(path.join(projectRoot, 'node_modules', 'electron', 'dist'), packageRoot, { recursive: true });
  await rename(path.join(packageRoot, 'electron.exe'), executablePath);

  await mkdir(stagingRoot, { recursive: true });
  await cp(path.join(projectRoot, '.vite'), path.join(stagingRoot, '.vite'), { recursive: true });
  const sourcePackage = JSON.parse(await readFile(path.join(projectRoot, 'package.json'), 'utf8'));
  const runtimePackage = {
    name: sourcePackage.name,
    productName: sourcePackage.productName,
    version: sourcePackage.version,
    description: sourcePackage.description,
    main: sourcePackage.main,
    author: sourcePackage.author,
    license: sourcePackage.license,
  };
  await writeFile(path.join(stagingRoot, 'package.json'), `${JSON.stringify(runtimePackage, null, 2)}\n`, 'utf8');

  await cp(path.join(projectRoot, 'Templates'), path.join(resourcesRoot, 'Templates'), { recursive: true });
  if (configuration === 'Release') {
    await createPackage(stagingRoot, path.join(resourcesRoot, 'app.asar'));
    await rm(stagingRoot, { recursive: true, force: true });
  } else {
    await rename(stagingRoot, path.join(resourcesRoot, 'app'));
  }
  await rm(path.join(resourcesRoot, 'default_app.asar'), { force: true });

  await flipFuses(executablePath, {
    version: FuseVersion.V1,
    [FuseV1Options.RunAsNode]: false,
    [FuseV1Options.EnableCookieEncryption]: true,
    [FuseV1Options.EnableNodeOptionsEnvironmentVariable]: false,
    [FuseV1Options.EnableNodeCliInspectArguments]: configuration !== 'Release',
    [FuseV1Options.EnableEmbeddedAsarIntegrityValidation]: configuration === 'Release',
    [FuseV1Options.OnlyLoadAppFromAsar]: configuration === 'Release',
  });
  return executablePath;
}

/** 引数を検証し、Node.js 24でも完了を保証できる構成別ビルドを実行する。 */
async function RunBuild() {
  const configuration = process.argv[2];
  if (!VALID_CONFIGURATIONS.has(configuration)) {
  console.error('ビルド構成はDebug、Development、Releaseのいずれかを指定してください。');
    return 1;
  }

  const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
  const projectRoot = path.resolve(scriptDirectory, '..');
  process.env.FBZZ_BUILD_CONFIG = configuration;

  try {
    await BuildVite(projectRoot);
    const executablePath = await AssemblePackage(projectRoot, configuration);
    console.log(`${configuration}ビルドを生成しました: ${executablePath}`);
    return 0;
  } catch (error) {
    const message = error instanceof Error ? error.stack ?? error.message : String(error);
    console.error(`${configuration}ビルドに失敗しました。\n${message}`);
    return 1;
  }
}

process.exitCode = await RunBuild();
