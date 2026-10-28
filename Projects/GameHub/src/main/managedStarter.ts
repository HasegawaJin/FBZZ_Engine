/**
 * @file managedStarter.ts
 * @brief Add the selected SDK's canonical C# starter without requiring Engine sources.
 * @author Hasegawa Jin
 * @date 2026-10-05
 */
import { access, mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import type { SdkBuildConfiguration } from '../shared/contracts';

const STARTER_FILES = ['Starter.Scripts.csproj', 'Assets/Scripts/Managed/MoveForwardComponent.cs', 'AGENTS.md', 'global.json', '.vscode/launch.json'] as const;
export type ManagedStarter = ReadonlyMap<string, string>;

export async function PrepareManagedStarter(sdkRoot: string,
  configuration: SdkBuildConfiguration): Promise<ManagedStarter> {
  return LoadManagedStarter(sdkRoot, configuration);
}

/** @brief Read and validate everything before GameHub creates a project directory. */
export async function LoadManagedStarter(sdkRoot: string, configuration: SdkBuildConfiguration): Promise<ManagedStarter> {
  if (!sdkRoot || !['Debug', 'Development', 'Release'].includes(configuration)) {
    throw new Error('C# starter requires a selected SDK and build configuration.');
  }
  const starterRoot = path.join(sdkRoot, 'share/fbzz/managed/ManagedStarter');
  const files = await Promise.all(STARTER_FILES.map(async (relative) => [relative, await readFile(path.join(starterRoot, relative), 'utf8')] as const));
  for (const name of ['Fbzz.ScriptApi.dll', 'Fbzz.ScriptApi.xml', 'Fbzz.ScriptGenerator.dll']) {
    await access(path.join(sdkRoot, 'bin', configuration, 'Managed', name));
  }
  const compiler = await readFile(path.join(sdkRoot, 'Projects/Managed/global.json'), 'utf8');
  if (files.find(([relative]) => relative === 'global.json')?.[1] !== compiler) {
    throw new Error('C# starter and SDK compiler selection disagree. Re-publish the SDK.');
  }
  return new Map(files);
}

function XmlProperty(value: string): string {
  /// @note MSBuild escaping prevents SDK folder characters from becoming expressions/item separators.
  return value.replace(/[%'$@;?*()]/g, (character) => `%${character.charCodeAt(0).toString(16).toUpperCase()}`)
    .replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
}

export async function WriteManagedStarter(projectRoot: string, targetName: string, sdkRoot: string,
  configuration: SdkBuildConfiguration, starter: ManagedStarter): Promise<void> {
  if (!/^[A-Za-z][A-Za-z0-9_]*$/.test(targetName)) throw new Error('Invalid C# project name.');
  const files = [...starter].map(([relative, text]) => [
    relative === 'Starter.Scripts.csproj' ? `${targetName}.Scripts.csproj` : relative, text,
  ] as const);
  files.push(['Fbzz.Sdk.props', `<Project>\n  <PropertyGroup>\n    <FbzzSdkRoot Condition="'$(FbzzSdkRoot)' == ''">${XmlProperty(path.resolve(sdkRoot))}</FbzzSdkRoot>\n    <FbzzSdkConfiguration Condition="'$(FbzzSdkConfiguration)' == ''">${configuration}</FbzzSdkConfiguration>\n  </PropertyGroup>\n</Project>\n`]);
  /// @note Refuse overwrites before the first write, including an existing compiler pin/sample.
  for (const [relative] of files) {
    try { await access(path.join(projectRoot, relative)); } catch { continue; }
    throw new Error(`C# starter would overwrite ${relative}.`);
  }
  for (const [relative, text] of files) {
    const destination = path.join(projectRoot, relative);
    await mkdir(path.dirname(destination), { recursive: true });
    await writeFile(destination, text, { encoding: 'utf8', flag: 'wx' });
  }
}
