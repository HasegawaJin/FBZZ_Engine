// FBZZ Engine
// AssembleDistribution.mjs | GameHub
// 公開済みGameHubとimmutable SDKを一つの配布ディレクトリへ集約する

import { access, cp, mkdir, readFile, readdir, rm, stat, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parse } from 'smol-toml';

const VALID_CONFIGURATIONS = new Set(['Debug', 'Development', 'Release']);

async function Exists(target) {
  try { await access(target); return true; } catch { return false; }
}

/** 指定構成を含むSDKだけを列挙し、publish時刻が最も新しいimmutable artifactを選ぶ。 */
async function FindLatestSdk(sdkStoreRoot, configuration) {
  const candidates = [];
  for (const entry of await readdir(sdkStoreRoot, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const root = path.join(sdkStoreRoot, entry.name);
    const manifestPath = path.join(root, 'fbzz-sdk.toml');
    const editorPath = path.join(root, 'tools', configuration, 'Editor', 'FBZZEditor.exe');
    if (!await Exists(manifestPath) || !await Exists(editorPath)) continue;
    const document = parse(await readFile(manifestPath, 'utf8'));
    const sdkId = typeof document.sdk?.id === 'string' ? document.sdk.id : entry.name;
    candidates.push({ root, sdkId, publishedAt: (await stat(manifestPath)).mtimeMs });
  }
  candidates.sort((left, right) => right.publishedAt - left.publishedAt);
  return candidates[0] ?? null;
}

async function Run() {
  const configuration = process.argv[2];
  if (!VALID_CONFIGURATIONS.has(configuration)) {
    console.error('配布構成はDebug、Development、Releaseのいずれかを指定してください。');
    return 1;
  }

  const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
  const repositoryRoot = path.resolve(projectRoot, '..', '..');
  const gameHubRoot = path.join(projectRoot, 'out', configuration, 'FBZZGameHub-win32-x64');
  const sdkStoreRoot = path.join(repositoryRoot, 'SDK');
  if (!await Exists(gameHubRoot)) throw new Error(`GameHub buildがありません: ${gameHubRoot}`);
  if (!await Exists(sdkStoreRoot)) throw new Error(`SDK storeがありません: ${sdkStoreRoot}`);

  const sdk = await FindLatestSdk(sdkStoreRoot, configuration);
  if (!sdk) throw new Error(`${configuration}構成を含む公開済みSDKがありません。`);

  const distributionRoot = path.join(repositoryRoot, 'Artifacts', 'Distribution', sdk.sdkId, configuration);
  const artifactsRoot = path.join(repositoryRoot, 'Artifacts');
  if (!path.resolve(distributionRoot).startsWith(`${path.resolve(artifactsRoot)}${path.sep}`)) {
    throw new Error('配布先がArtifacts外へ解決されたため処理を中止しました。');
  }

  await rm(distributionRoot, { recursive: true, force: true });
  await mkdir(distributionRoot, { recursive: true });
  await Promise.all([
    cp(gameHubRoot, path.join(distributionRoot, 'GameHub'), { recursive: true }),
    cp(sdk.root, path.join(distributionRoot, 'SDK', sdk.sdkId), { recursive: true }),
  ]);
  await writeFile(path.join(distributionRoot, 'distribution.json'), `${JSON.stringify({
    sdkId: sdk.sdkId,
    configuration,
    gameHub: 'GameHub/FBZZGameHub.exe',
    sdk: `SDK/${sdk.sdkId}`,
  }, null, 2)}\n`, 'utf8');
  console.log(`配布物を生成しました: ${distributionRoot}`);
  return 0;
}

try {
  process.exitCode = await Run();
} catch (error) {
  console.error(error instanceof Error ? error.stack ?? error.message : String(error));
  process.exitCode = 1;
}
