/// @file    managedStarter.ts
/// @brief   Add the selected SDK's canonical C# starter without requiring Engine sources.
/// @author  Hasegawa Jin
/// @date    2026-10-05
import { access, mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import type { SdkBuildConfiguration } from '../shared/contracts';

const STARTER_FILES = ['Starter.Scripts.csproj', 'Fbzz.ScriptBuild.props', 'Assets/Scripts/Managed/MoveForwardComponent.cs', 'AGENTS.md', 'global.json', '.vscode/launch.json'] as const;
export type ManagedStarter = ReadonlyMap<string, string>;

export async function PrepareManagedStarter(sdkRoot: string,
  configuration: SdkBuildConfiguration): Promise<ManagedStarter> {
  return LoadManagedStarter(sdkRoot, configuration);
}

/// @brief Read and validate everything before GameHub creates a project directory.
export async function LoadManagedStarter(sdkRoot: string, configuration: SdkBuildConfiguration): Promise<ManagedStarter> {
  const hasSupportedConfiguration = ['Debug', 'Development', 'Release'].includes(configuration);
  if (!sdkRoot || !hasSupportedConfiguration) {
    throw new Error('C# starter requires a selected SDK and build configuration.');
  }

  // 雛形の全ファイルと、選んだ構成で必要な SDK の成果物が揃っていることを確認する。
  const starterRoot = path.join(sdkRoot, 'share/fbzz/managed/ManagedStarter');
  const files = new Map(await Promise.all(STARTER_FILES.map(async (relativePath) => {
    const text = await readFile(path.join(starterRoot, relativePath), 'utf8');
    return [relativePath, text] as const;
  })));
  for (const name of ['Fbzz.ScriptApi.dll', 'Fbzz.ScriptApi.xml', 'Fbzz.ScriptGenerator.dll']) {
    await access(path.join(sdkRoot, 'bin', configuration, 'Managed', name));
  }
  // 作成したプロジェクトと SDK が同じ .NET SDK の選択設定を使うようにする。
  const compilerPin = await readFile(path.join(sdkRoot, 'Projects/Managed/global.json'), 'utf8');
  if (files.get('global.json') !== compilerPin) {
    throw new Error('C# starter and SDK compiler selection disagree. Re-publish the SDK.');
  }
  return files;
}

function XmlProperty(value: string): string {
  // SDK パスの文字が MSBuild の式や項目区切りとして解釈されないようにする。
  return value.replace(/[%'$@;?*()]/g, (character) => `%${character.charCodeAt(0).toString(16).toUpperCase()}`)
    .replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
}

export async function WriteManagedStarter(projectRoot: string, targetName: string, sdkRoot: string,
  configuration: SdkBuildConfiguration, starter: ManagedStarter): Promise<void> {
  if (!/^[A-Za-z][A-Za-z0-9_]*$/.test(targetName)) {
    throw new Error('Invalid C# project name.');
  }

  // ファイル名にプロジェクト名を反映し、選択した SDK を参照する設定を加える。
  const files = [...starter].map(([relative, text]) => [
    relative === 'Starter.Scripts.csproj' ? `${targetName}.Scripts.csproj` : relative, text,
  ] as const);
  files.push(['Fbzz.Sdk.props', `<Project>\n  <PropertyGroup>\n    <FbzzSdkRoot Condition="'$(FbzzSdkRoot)' == ''">${XmlProperty(path.resolve(sdkRoot))}</FbzzSdkRoot>\n    <FbzzSdkConfiguration Condition="'$(FbzzSdkConfiguration)' == ''">${configuration}</FbzzSdkConfiguration>\n  </PropertyGroup>\n</Project>\n`]);
  // 書き込みを始める前に、既存の設定やサンプルを上書きしないことを確認する。
  for (const [relative] of files) {
    try {
      await access(path.join(projectRoot, relative));
    } catch {
      continue;
    }
    throw new Error(`C# starter would overwrite ${relative}.`);
  }

  // 事前確認の後に同名ファイルが作られても、排他的な新規作成で既存の内容を保護する。
  for (const [relative, text] of files) {
    const destination = path.join(projectRoot, relative);
    await mkdir(path.dirname(destination), { recursive: true });
    await writeFile(destination, text, { encoding: 'utf8', flag: 'wx' });
  }
}
