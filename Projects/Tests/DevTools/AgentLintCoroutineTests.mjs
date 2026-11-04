/// @file    AgentLintCoroutineTests.mjs
/// @brief   Coroutine 実装だけを許可し、利用側の独自中断と標準 ABI を拒否する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import test from 'node:test';
import {LintContent} from '../../DevTools/AgentLint/lint.mjs';

const backend='Projects/Engine/src/Scene/CoroutineFrameLoop.cpp';
const runtime='Projects/Engine/src/Scene/ScriptRuntimeProxies.cpp';
const header='Projects/Engine/include/Engine/Scene/Coroutine.hpp';
const findings=(path,source)=>LintContent(path,source).filter(f=>/coroutine/.test(f.rule));
const read=path=>readFileSync(fileURLToPath(new URL('../../../'+path,import.meta.url)),'utf8');

test('The production Coroutine implementation passes without suppressing other rules',()=>{
    assert.deepEqual(findings(backend,read(backend)),[]);
    assert.deepEqual(findings(header,read(header)),[]);
    assert.ok(LintContent(backend,'void Bad() { throw 1; }').some(f=>f.rule==='no-throw'));
});
test('Tween has no suspension constructs to exempt',()=>{
    assert.deepEqual(findings(runtime,read(runtime)),[]);
});
test('Including the Engine Coroutine does not exempt its callers',()=>{
    for(const keyword of ['co_await WaitForFrames(1)','co_return','co_yield 1']) {
        assert.ok(findings(runtime,'#include <Engine/Scene/Coroutine.hpp>\nvoid Bad(){ '+keyword+'; }')
            .some(f=>f.rule==='no-coroutine'));
    }
});
test('Copying the implementation to another path is rejected',()=>{
    assert.ok(findings('Projects/Engine/src/Scene/AnotherCoroutine.cpp',read(backend))
        .some(f=>f.rule==='no-coroutine'));
});
test('Independent standard coroutine machinery is rejected',()=>{
    assert.ok(findings(runtime,'#include <coroutine>').some(f=>f.rule==='no-coroutine-header'));
    assert.ok(findings(runtime,'#include "coroutine"').some(f=>f.rule==='no-coroutine-header'));
    assert.ok(findings(runtime,'std::coroutine_handle<> handle;').some(f=>f.rule==='no-native-coroutine'));
    assert.ok(findings(runtime,'std :: coroutine_handle<> handle;').some(f=>f.rule==='no-native-coroutine'));
    assert.ok(findings(runtime,'std::suspend_always Wait();').some(f=>f.rule==='no-native-coroutine'));
});
test('Comments and string literals are not suspension expressions',()=>{
    assert.deepEqual(findings(runtime,'/// @note co_await example\nconst char* text="co_return std::coroutine_handle";'),[]);
    assert.deepEqual(findings(runtime,'/*\n#include <coroutine>\n*/'),[]);
    assert.deepEqual(findings(runtime,'const char* text=R"(\n#include <coroutine>\n)";'),[]);
});
