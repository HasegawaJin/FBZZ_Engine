---
name: add-script-proxy
description: ゲームスクリプト (GreenWare/Assets/Scripts) から新しいエンジン機能を呼べるよう ScriptProxy を追加・拡張する手順。「スクリプトから〇〇を操作したい」「プロキシに関数を足す」「Script から Engine の型を使いたい」のときに使う。スクリプトから Engine 実装型を直接 include するのは禁止なので必ずこの経路を通す。
---

# ScriptProxy の追加

スクリプトは SDK 越しの DLL なので、Engine 内部型を見せない薄い窓口 (プロキシ) だけを通す。

## 既存プロキシに関数を足す

1. `Projects/Engine/include/Engine/Scene/ScriptProxy/ScriptXxxProxy.hpp` に宣言。引数・戻り値は `math::` の値型・`std::string`・プリミティブ・既存プロキシ型だけ (Engine 内部型を露出しない)。単位と失敗時の振る舞いを `@note` / `@return` に書く
2. `Projects/Engine/src/Scene/ScriptProxies.cpp` の該当プロキシ節に実装。`script` が nullptr / GameObject が無いときは何もしない・既定値を返す (落とさない)

## 新しいプロキシを作る

上の 2 つに加えて:

3. `Projects/Engine/include/Engine/Scene/ScriptProxy/ScriptProxyMembers.inl` に `FBZZ_PROXY_MEMBER(ScriptXxxProxy, xxx)` を 1 行 (メンバー名の正本)
4. `Projects/Engine/include/Engine/Scene/ScriptProxy/AllScriptProxies.hpp` に include を 1 行
5. メンバー名は `lifetime` / `time` / `random` 等の既存名と衝突させない (スクリプト側で再宣言すると基底が隠れる)

## ABI

- `Script` 基底に仮想関数を足すときは vtable 末尾に置き、`ScriptDllAbi.hpp` の `kScriptVtableAbiVersion` を上げる
- プロキシ構造体のメンバー追加も SDK の再発行が要る。スクリプト DLL は SDK → Scripts の順でビルドされる

## 検証

```
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 check Projects/Engine/src/Scene/ScriptProxies.cpp Projects/Engine/include/Engine/Scene/ScriptProxy/ScriptXxxProxy.hpp
```

呼び出し側のスクリプトの確認は SDK の再発行とスクリプト DLL のビルドが要るので、ユーザーに `GameHub + SDK: Build` タスクを依頼する。
