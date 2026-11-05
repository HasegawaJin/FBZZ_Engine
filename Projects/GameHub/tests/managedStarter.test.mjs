/// @file    managedStarter.test.mjs
/// @brief   Verify C# project generation, launch inputs and published SDK authoring contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-05
import assert from 'node:assert/strict';
import { access, cp, mkdir, mkdtemp, readFile, readdir, rename, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { LoadManagedStarter, PrepareManagedStarter, WriteManagedStarter } from '../src/main/managedStarter.ts';
import { createHash } from 'node:crypto';
import { AssertManagedAuthoringContract } from '../src/shared/sdkManagedContract.mjs';
import { build } from 'esbuild';
import { parse } from 'smol-toml';
import { ReadEngineVersion } from '../scripts/EngineVersion.mjs';

const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const engineVersion = ReadEngineVersion(repository);
const gameHubRoot = path.join(repository, 'Projects/GameHub');
const services = build({
  stdin: {
    contents: 'export { TemplateService } from "./src/main/templateService.ts"; export { ProjectService } from "./src/main/projectService.ts";',
    resolveDir: gameHubRoot,
  },
  bundle: true,
  write: false,
  platform: 'node',
  format: 'esm',
  define: {
    __FBZZ_ENGINE_VERSION__: JSON.stringify(engineVersion),
    __FBZZ_BUILD_CONFIGURATION__: JSON.stringify('Development'),
    'process.resourcesPath': JSON.stringify(gameHubRoot),
  },
  plugins: [{
    name: 'electron-test-host',
    setup(builder) {
      builder.onResolve({ filter: /^electron$/ }, () => ({ path: 'electron', namespace: 'test-host' }));
      builder.onLoad({ filter: /.*/, namespace: 'test-host' }, () => ({
        contents: `export const app = { getAppPath: () => ${JSON.stringify(gameHubRoot)} }; export const shell = {}; export const net = {};`,
      }));
    },
  }],
}).then((result) => import(`data:text/javascript;base64,${Buffer.from(result.outputFiles[0].text).toString('base64')}`));

async function Fixture(run) {
  const root = await mkdtemp(path.join(tmpdir(), 'fbzz-managed-starter-'));
  try {
    const sdk = path.join(root, 'SDK & published');
    await cp(path.join(repository, 'Projects/Managed/Authoring/ManagedStarter'), path.join(sdk, 'share/fbzz/managed/ManagedStarter'),
      { recursive: true, filter: (source) => !['bin', 'obj', 'Library'].includes(path.basename(source)) });
    const compiler = await readFile(path.join(repository, 'Projects/Managed/global.json'), 'utf8');
    await mkdir(path.join(sdk, 'Projects/Managed'), { recursive: true });
    await writeFile(path.join(sdk, 'Projects/Managed/global.json'), compiler);
    await writeFile(path.join(sdk, 'share/fbzz/managed/ManagedStarter/global.json'), compiler);
    await mkdir(path.join(sdk, 'bin/Development/Managed'), { recursive: true });
    for (const name of ['Fbzz.ScriptApi.dll', 'Fbzz.ScriptApi.xml', 'Fbzz.ScriptGenerator.dll']) {
      await writeFile(path.join(sdk, 'bin/Development/Managed', name), 'fixture: not executed');
    }
    await run(root, sdk);
  } finally {
    assert.ok(path.resolve(root).startsWith(`${path.resolve(tmpdir())}${path.sep}fbzz-managed-starter-`));
    await rm(root, { recursive: true, force: true });
  }
}

async function PublishedManagedFixture(configuration, run) {
  const parent = path.join(repository, 'Scratch/CSharpRedesign/PackagingProbes');
  await mkdir(parent, { recursive: true });
  const root = await mkdtemp(path.join(parent, 'owned-'));
  const files = {};
  const shared = ['Tools/AgentBuild.ps1', 'Projects/Managed/global.json',
    'share/fbzz/managed/ManagedStarter/Starter.Scripts.csproj',
    'share/fbzz/managed/ManagedStarter/Fbzz.ScriptBuild.props',
    'share/fbzz/managed/ManagedStarter/Assets/Scripts/Managed/MoveForwardComponent.cs',
    'share/fbzz/managed/ManagedStarter/AGENTS.md', 'share/fbzz/managed/ManagedStarter/.vscode/launch.json',
    'share/fbzz/managed/ManagedStarter/global.json'];
  const managed = ['Fbzz.ScriptApi.dll', 'Fbzz.ScriptApi.xml', 'Fbzz.ScriptGenerator.dll',
    'Fbzz.ManagedBuildTasks.dll', 'ManagedIncremental.targets', 'Fbzz.ScriptBuild.props', 'Fbzz.ManagedHost.dll',
    'Fbzz.ManagedHost.runtimeconfig.json', 'Fbzz.ManagedHost.deps.json'];
  try {
    for (const relative of [...shared, ...['nethost.dll', ...managed.map((name) => `Managed/${name}`)].flatMap((name) => [
      `bin/${configuration}/${name}`, `tools/${configuration}/Editor/${name}`,
    ])]) {
      const text = relative.endsWith('global.json') ? 'compiler-pin' : `published:${path.basename(relative)}`;
      await mkdir(path.dirname(path.join(root, relative)), { recursive: true });
      await writeFile(path.join(root, relative), text);
      files[relative] = createHash('sha256').update(text).digest('hex');
    }
    await run(root, files);
  } finally {
    assert.ok(path.resolve(root).startsWith(`${path.resolve(parent)}${path.sep}owned-`));
    await rm(root, { recursive: true, force: true });
  }
}

for (const templateId of ['empty', 'standard']) {
  test(`${templateId} creates a launchable C# project without native Script scaffolding`, async () => {
    const { TemplateService, ProjectService } = await services;
    await Fixture(async (root, sdk) => {
      const project = await new TemplateService().create({
        displayName: 'CSharp Game', destinationRoot: root, templateId,
      }, engineVersion, sdk, 'Development');
      const entry = await new ProjectService().inspectProject({ path: project, lastOpened: '' });
      assert.equal(entry.projectFileValid, true);
      assert.equal(entry.layoutValid, true);
      assert.equal(entry.migrationRequired, false);
      const projectText = await readFile(path.join(project, '.fbzz_proj'), 'utf8');
      assert.doesNotMatch(projectText, /\{\{/);
      const metadata = parse(projectText).project;
      await access(path.resolve(project, metadata.default_scene));
      assert.equal(metadata.build_root, 'Build/VS/');
      for (const retired of ['api_root', 'public_api_header', 'script_root', 'cpp_namespace', 'library_root']) {
        assert.equal(metadata[retired], undefined);
      }
      for (const relative of ['CSharpGame.Scripts.csproj', 'Fbzz.Sdk.props', 'Fbzz.ScriptBuild.props',
        'global.json', '.vscode/launch.json', 'Assets/Scripts/Managed/MoveForwardComponent.cs']) {
        await access(path.join(project, relative));
      }
      for (const retired of ['Include', 'Src/Scripts', 'Lib']) {
        await assert.rejects(access(path.join(project, retired)), /ENOENT/, retired);
      }
      const cmake = await readFile(path.join(project, 'CMakeLists.txt'), 'utf8');
      assert.doesNotMatch(cmake, /ScriptsDll|ScriptModules|REGISTER_SCRIPT|PRIVATE Assets Include Src/);
    });
  });
}

test('C# project inspection requires launch settings and scene rather than native authoring folders', async () => {
  const { TemplateService, ProjectService } = await services;
  await Fixture(async (root, sdk) => {
    const project = await new TemplateService().create({
      displayName: 'LaunchFiles', destinationRoot: root, templateId: 'empty',
    }, engineVersion, sdk, 'Development');
    const service = new ProjectService();
    await rename(path.join(project, 'Src'), path.join(project, 'NativeStart.saved'));
    const inspect = () => service.inspectProject({ path: project, lastOpened: '' });
    assert.equal((await inspect()).layoutValid, true);
    for (const relative of ['ProjectSettings/ProjectSettings.toml', 'Assets/Scenes/Main.scene']) {
      const original = path.join(project, relative);
      await rename(original, `${original}.saved`);
      assert.equal((await inspect()).layoutValid, false, relative);
      await rename(`${original}.saved`, original);
      assert.equal((await inspect()).layoutValid, true, relative);
    }
    const metadataPath = path.join(project, '.fbzz_proj');
    const metadata = await readFile(metadataPath, 'utf8');
    const oldSettings = path.join(root, 'MovedProject/ProjectSettings/ProjectSettings.toml').replaceAll('\\', '/');
    await writeFile(metadataPath, metadata.replace('ProjectSettings/ProjectSettings.toml', oldSettings));
    assert.equal((await inspect()).layoutValid, true);
  });
});

test('each advertised configuration needs its own complete published managed authoring pair', async () => {
  for (const configuration of ['Debug', 'Development', 'Release']) {
    await PublishedManagedFixture(configuration, async (root, files) => {
      await AssertManagedAuthoringContract(root, configuration, files);
      await assert.rejects(AssertManagedAuthoringContract(root, configuration === 'Debug' ? 'Release' : 'Debug', files), /records disagree/);
      for (const relative of [`bin/${configuration}/Managed/Fbzz.ManagedBuildTasks.dll`,
        `tools/${configuration}/Editor/Managed/ManagedIncremental.targets`,
        `tools/${configuration}/Editor/Managed/Fbzz.ManagedHost.runtimeconfig.json`,
        `tools/${configuration}/Editor/Managed/Fbzz.ManagedHost.deps.json`]) {
        const original = await readFile(path.join(root, relative));
        await rm(path.join(root, relative));
        await assert.rejects(AssertManagedAuthoringContract(root, configuration, files), /ENOENT/);
        await writeFile(path.join(root, relative), original);
      }
    });
  }
});

test('same metadata with stale bytes and mismatched published pairs cannot advertise managed support', async () => {
  await PublishedManagedFixture('Release', async (root, files) => {
    const host = 'tools/Release/Editor/Managed/Fbzz.ManagedHost.dll';
    await writeFile(path.join(root, host), 'older ABI');
    await assert.rejects(AssertManagedAuthoringContract(root, 'Release', files), /SHA256 mismatch/);
    files[host] = createHash('sha256').update('older ABI').digest('hex');
    await assert.rejects(AssertManagedAuthoringContract(root, 'Release', files), /records disagree/);
  });
  await PublishedManagedFixture('Development', async (root, files) => {
    files['share/fbzz/managed/ManagedStarter/global.json'] = 'a'.repeat(64);
    await assert.rejects(AssertManagedAuthoringContract(root, 'Development', files), /compiler pin records disagree/);
  });
});

test('selected SDK creates a project from canonical sources and escapes local SDK properties', async () => {
  await Fixture(async (root, sdk) => {
    const starter = await LoadManagedStarter(sdk, 'Development');
    const project = path.join(root, 'MyGame');
    await WriteManagedStarter(project, 'MyGame', sdk, 'Development', starter);
    assert.equal(await readFile(path.join(project, 'MyGame.Scripts.csproj'), 'utf8'), starter.get('Starter.Scripts.csproj'));
    assert.equal(await readFile(path.join(project, 'Fbzz.ScriptBuild.props'), 'utf8'), starter.get('Fbzz.ScriptBuild.props'));
    assert.equal(await readFile(path.join(project, 'Assets/Scripts/Managed/MoveForwardComponent.cs'), 'utf8'), starter.get('Assets/Scripts/Managed/MoveForwardComponent.cs'));
    assert.equal(await readFile(path.join(project, 'global.json'), 'utf8'), starter.get('global.json'));
    assert.equal(await readFile(path.join(project, '.vscode/launch.json'), 'utf8'), starter.get('.vscode/launch.json'));
    const props = await readFile(path.join(project, 'Fbzz.Sdk.props'), 'utf8');
    assert.ok(props.includes('SDK &amp; published'));
    assert.ok(props.includes('<FbzzSdkConfiguration Condition="\'$(FbzzSdkConfiguration)\' == \'\'">Development'));
    assert.ok(!(await readdir(project)).includes('Starter.Scripts.csproj'));
  });
});

test('missing generator and mismatched compiler pin fail before project creation', async () => {
  await Fixture(async (_root, sdk) => {
    await rm(path.join(sdk, 'bin/Development/Managed/Fbzz.ScriptGenerator.dll'));
    await assert.rejects(LoadManagedStarter(sdk, 'Development'), /ENOENT/);
    await writeFile(path.join(sdk, 'bin/Development/Managed/Fbzz.ScriptGenerator.dll'), 'fixture');
    await writeFile(path.join(sdk, 'share/fbzz/managed/ManagedStarter/global.json'), '{}');
    await assert.rejects(LoadManagedStarter(sdk, 'Development'), /compiler selection disagree/);
  });
});

test('existing user files prevent every starter write', async () => {
  await Fixture(async (root, sdk) => {
    const project = path.join(root, 'MyGame');
    await mkdir(project);
    await writeFile(path.join(project, 'global.json'), 'user compiler pin');
    await assert.rejects(WriteManagedStarter(project, 'MyGame', sdk, 'Development', await LoadManagedStarter(sdk, 'Development')), /overwrite global.json/);
    assert.deepEqual(await readdir(project), ['global.json']);
    assert.equal(await readFile(path.join(project, 'global.json'), 'utf8'), 'user compiler pin');
  });
});
