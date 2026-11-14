/// @file    BuildGameHub.mjs
/// @brief   SDK の最新性を検証し、Vite 生成物と Electron ランタイムを構成別パッケージへ組み立てる。
/// @author  Hasegawa Jin
/// @date    2026/07/20

import { createHash } from 'node:crypto';
import { cp, mkdir, readFile, rename, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { build as BuildViteBundle } from 'vite';
import { AssertSdkIsCurrent } from './SdkFreshness.mjs';

const require = createRequire(import.meta.url);
const ViteConfigGenerator = require('@electron-forge/plugin-vite/dist/ViteConfig.js').default;
const { createPackage, getRawHeader } = require('@electron/asar');
const { flipFuses, FuseV1Options, FuseVersion } = require('@electron/fuses');
const { Data, NtExecutable, NtExecutableResource, Resource } = require('resedit');
const VALID_CONFIGURATIONS = new Set(['Debug', 'Development', 'Release']);

/// @see https://github.com/electron/electron/blob/main/shell/common/asar/archive_win.cc Electron が照合する ASAR リソース。
const INTEGRITY_RESOURCE_TYPE = 'INTEGRITY';
const INTEGRITY_RESOURCE_ID = 'ELECTRONASAR';

/// @note 開発サーバーと同じ Forge Vite Plugin の基底設定を使う。
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

/// @note EnableEmbeddedAsarIntegrityValidation を有効にする Release では起動時にこのリソースが必須。
function AddAsarIntegrity(entries, packageRoot, asarPath) {
  const { headerString } = getRawHeader(asarPath);
  const integrity = [{
    file: path.win32.relative(packageRoot, asarPath),
    alg: 'SHA256',
    value: createHash('SHA256').update(headerString).digest('hex'),
  }];

  const versionInfo = Resource.VersionInfo.fromEntries(entries);
  if (versionInfo.length !== 1) {
    throw new Error('Electron本体のバージョンリソースを特定できませんでした。');
  }
  const languages = versionInfo[0].getAllLanguagesForStringValues();
  if (languages.length !== 1) {
    throw new Error('Electron本体の言語リソースを特定できませんでした。');
  }

  entries.push({
    type: INTEGRITY_RESOURCE_TYPE,
    id: INTEGRITY_RESOURCE_ID,
    bin: Buffer.from(JSON.stringify(integrity), 'utf-8'),
    lang: languages[0].lang,
    codepage: languages[0].codepage,
  });
}

/// @note Fuse の変更前に PE リソースを更新する。resedit による再生成は Fuse の変更を保持しない。
/// @see https://github.com/electron/packager/blob/main/src/resedit.ts Electron Packager の既存アイコングループ置換。
async function WriteExecutableResources(executablePath, iconPath, packageRoot, asarPath) {
  const executable = NtExecutable.from(await readFile(executablePath));
  const resources = NtExecutableResource.from(executable);
  const iconGroups = Resource.IconGroupEntry.fromEntries(resources.entries);
  if (iconGroups.length !== 1) {
    throw new Error('Electron本体のアイコングループを特定できませんでした。');
  }

  const [iconGroup] = iconGroups;
  const icon = Data.IconFile.from(await readFile(iconPath));
  Resource.IconGroupEntry.replaceIconsForResource(
    resources.entries, iconGroup.id, iconGroup.lang, icon.icons.map((item) => item.data),
  );
  if (asarPath) {
    AddAsarIntegrity(resources.entries, packageRoot, asarPath);
  }
  resources.outputResource(executable);
  await writeFile(executablePath, Buffer.from(executable.generate()));
}

async function AssemblePackage(projectRoot, configuration) {
  const configurationRoot = path.join(projectRoot, 'out', configuration);
  const packageRoot = path.join(configurationRoot, 'FBZZGameHub-win32-x64');
  const stagingRoot = path.join(configurationRoot, '.app-staging');
  const resourcesRoot = path.join(packageRoot, 'resources');
  const executablePath = path.join(packageRoot, 'FBZZGameHub.exe');
  const iconPath = path.join(projectRoot, 'Branding', 'GameHub.ico');
  const asarPath = configuration === 'Release' ? path.join(resourcesRoot, 'app.asar') : null;

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

  const templatesRoot = path.join(projectRoot, 'Templates');
  const generatedTemplateRoots = new Set(['Binaries', 'Build', 'Lib', 'Library']);
  await cp(templatesRoot, path.join(resourcesRoot, 'Templates'), {
    recursive: true,
    // ローカル生成物は配布templateへ梱包しない。Engine shader はテンプレートに存在しない (SDK の share を使う)。
    filter: (source) => {
      const relativeParts = path.relative(templatesRoot, source).split(path.sep);
      return !(relativeParts.length >= 2 && generatedTemplateRoots.has(relativeParts[1]));
    },
  });
  if (asarPath) {
    await createPackage(stagingRoot, asarPath);
    await rm(stagingRoot, { recursive: true, force: true });
  } else {
    await rename(stagingRoot, path.join(resourcesRoot, 'app'));
  }
  await rm(path.join(resourcesRoot, 'default_app.asar'), { force: true });

  await WriteExecutableResources(executablePath, iconPath, packageRoot, asarPath);
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

async function RunBuild() {
  const configuration = process.argv[2];
  if (!VALID_CONFIGURATIONS.has(configuration)) {
    console.error('ビルド構成はDebug、Development、Releaseのいずれかを指定してください。');
    return 1;
  }

  const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
  const projectRoot = path.resolve(scriptDirectory, '..');
  const repositoryRoot = path.resolve(projectRoot, '..', '..');
  process.env.FBZZ_BUILD_CONFIG = configuration;

  try {
    await AssertSdkIsCurrent(repositoryRoot, configuration);
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
