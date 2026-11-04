/// @file    sdkManagedContract.mjs
/// @brief   公開 manifest に対応する managed authoring 部品を両配置で確認する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import path from 'node:path';

const MANAGED_FILES = ['Fbzz.ScriptApi.dll', 'Fbzz.ScriptApi.xml', 'Fbzz.ScriptGenerator.dll',
  'Fbzz.ManagedBuildTasks.dll', 'ManagedIncremental.targets', 'Fbzz.ScriptBuild.props', 'Fbzz.ManagedHost.dll',
  'Fbzz.ManagedHost.runtimeconfig.json', 'Fbzz.ManagedHost.deps.json'];
const SHARED_FILES = ['Tools/AgentBuild.ps1', 'Projects/Managed/global.json',
  'share/fbzz/managed/ManagedStarter/Starter.Scripts.csproj',
  'share/fbzz/managed/ManagedStarter/Fbzz.ScriptBuild.props',
  'share/fbzz/managed/ManagedStarter/Assets/Scripts/Managed/MoveForwardComponent.cs',
  'share/fbzz/managed/ManagedStarter/AGENTS.md', 'share/fbzz/managed/ManagedStarter/.vscode/launch.json',
  'share/fbzz/managed/ManagedStarter/global.json'];

/// @note 構成の validated/fingerprint は呼出元の既存 SDK 契約で検査する。ABI の推測ではなく公開時の内容 hash を照合する。
export async function AssertManagedAuthoringContract(root, configuration, files) {
  if (!['Debug', 'Development', 'Release'].includes(configuration) || !files) throw new Error('SDK managed configuration is not published.');
  const pairs = ['nethost.dll', ...MANAGED_FILES.map((name) => `Managed/${name}`)].map((name) => [
    `bin/${configuration}/${name}`, `tools/${configuration}/Editor/${name}`,
  ]);
  for (const [bin, editor] of pairs) {
    if (!/^[0-9a-f]{64}$/.test(files[bin] ?? '') || files[bin] !== files[editor]) {
      throw new Error(`SDK managed publication records disagree: ${bin}`);
    }
  }
  if (files['Projects/Managed/global.json'] !== files['share/fbzz/managed/ManagedStarter/global.json']) {
    throw new Error('SDK and C# starter compiler pin records disagree.');
  }
  for (const relative of [...SHARED_FILES, ...pairs.flat()]) {
    const expected = files[relative];
    if (!/^[0-9a-f]{64}$/.test(expected ?? '')) throw new Error(`SDK managed publication hash missing: ${relative}`);
    const digest = createHash('sha256');
    for await (const chunk of createReadStream(path.join(root, relative))) digest.update(chunk);
    if (digest.digest('hex') !== expected) throw new Error(`SDK managed SHA256 mismatch: ${relative}`);
  }
}
