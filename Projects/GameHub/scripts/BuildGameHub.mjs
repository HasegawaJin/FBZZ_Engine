// FBZZ Engine
// BuildGameHub.mjs | GameHub
// SDKの最新性を検証し、Vite生成物とElectronランタイムを構成別パッケージへ組み立てる

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
const { NtExecutable, NtExecutableResource, Resource } = require('resedit');
const VALID_CONFIGURATIONS = new Set(['Debug', 'Development', 'Release']);

// exe内でasarハッシュを保持するリソース。Electronのarchive_win.cppがこの型とIDで探す。
const INTEGRITY_RESOURCE_TYPE = 'INTEGRITY';
const INTEGRITY_RESOURCE_ID = 'ELECTRONASAR';

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

/**
 * app.asarヘッダーのSHA-256をexeのリソースへ書き込む。
 *
 * WHY: EnableEmbeddedAsarIntegrityValidationを立てたexeは起動直後にこのリソースを
 *      FindResourceで引く。書き忘れると照合以前に «FindResource failed» のFATALで
 *      落ち、アプリのコードへ到達しない。@electron/packagerはこれをresedit工程で
 *      行うが、本スクリプトはElectron本体を手で組むためここで補う。
 */
async function WriteAsarIntegrity(executablePath, packageRoot, asarPath) {
  const { headerString } = getRawHeader(asarPath);
  const integrity = [{
    file: path.win32.relative(packageRoot, asarPath),
    alg: 'SHA256',
    value: createHash('SHA256').update(headerString).digest('hex'),
  }];

  const executable = NtExecutable.from(await readFile(executablePath));
  const resources = NtExecutableResource.from(executable);
  const versionInfo = Resource.VersionInfo.fromEntries(resources.entries);
  if (versionInfo.length !== 1) {
    throw new Error('Electron本体のバージョンリソースを特定できませんでした。');
  }
  const languages = versionInfo[0].getAllLanguagesForStringValues();
  if (languages.length !== 1) {
    throw new Error('Electron本体の言語リソースを特定できませんでした。');
  }

  resources.entries.push({
    type: INTEGRITY_RESOURCE_TYPE,
    id: INTEGRITY_RESOURCE_ID,
    bin: Buffer.from(JSON.stringify(integrity), 'utf-8'),
    lang: languages[0].lang,
    codepage: languages[0].codepage,
  });
  resources.outputResource(executable);
  await writeFile(executablePath, Buffer.from(executable.generate()));
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

  const templatesRoot = path.join(projectRoot, 'Templates');
  const sharedShaderRoot = path.join(templatesRoot, 'standard', 'Assets', 'Shaders');
  const sharedShaderMeta = `${sharedShaderRoot}.meta`;
  const generatedTemplateRoots = new Set(['Binaries', 'Build', 'Lib', 'Library']);
  await cp(templatesRoot, path.join(resourcesRoot, 'Templates'), {
    recursive: true,
    // Engine shaderとローカル生成物は配布templateへ梱包しない。
    filter: (source) => {
      const relativeParts = path.relative(templatesRoot, source).split(path.sep);
      if (relativeParts.length >= 2 && generatedTemplateRoots.has(relativeParts[1])) return false;
      return source !== sharedShaderMeta
        && source !== sharedShaderRoot
        && !source.startsWith(`${sharedShaderRoot}${path.sep}`);
    },
  });
  if (configuration === 'Release') {
    const asarPath = path.join(resourcesRoot, 'app.asar');
    await createPackage(stagingRoot, asarPath);
    await rm(stagingRoot, { recursive: true, force: true });
    // WHY flipFuses より前か: reseditはPE全体を作り直すため、後から掛けるとfuseの書き換えが消える。
    await WriteAsarIntegrity(executablePath, packageRoot, asarPath);
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
