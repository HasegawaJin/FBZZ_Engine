# スレッドモデル規約

---

## Step 1〜5: シングルスレッド

**Step 1〜5 のエンジンはシングルスレッドで動作する。**

ゲームループはメインスレッド 1 本で以下の順に実行される。

```
メインスレッド (毎フレーム)
│
├─ Window::PollEvents()       Win32 メッセージポンプ
├─ Input::Update()            入力状態更新
├─ Scene::Update(dt)          ゲームロジック
├─ World::Step(dt)            物理シミュレーション
├─ IRenderer::BeginFrame()
├─ Scene::Render(renderer)    描画コマンド発行
├─ DebugDraw::Flush()
└─ IRenderer::EndFrame()      Present
```

---

## 禁止事項 (Step 1〜5)

以下はエンジン・物理・数学レイヤーのコードに持ち込まない。

- `std::thread`
- `std::mutex` / `std::lock_guard` / `std::unique_lock`
- `std::atomic<T>`
- `std::condition_variable`
- Win32 の `CreateThread` / `_beginthreadex`

マルチスレッド関連のコードが必要になった場合は、先にユーザーへ確認する。

---

## Step 6 以降: レンダースレッド分離 (将来)

DX12 移行 (Step 6) でレンダースレッドを分離する可能性がある。
その際のデータフロー設計:

```
メインスレッド            レンダースレッド
│                         │
├─ Scene::Update()        │
├─ World::Step()          │
│                         │
└─ RenderProxy を書き込む ──→ RenderProxy を読み込んで描画
   (ダブルバッファ)
```

ゲームロジック (Scene / physics / math) とレンダリングのデータを
将来分離しやすくするために、Step 1〜5 から以下を意識しておく:

- `Scene::Render()` はゲーム状態を**読むだけ**で変更しない
- `World::Step()` はレンダリング用データを直接書き込まない

これはパフォーマンス最適化ではなく、Step 6 への設計的な準備。

---

## Win32 メッセージポンプ

Win32 ウィンドウメッセージはメインスレッドでのみ処理する。
`PeekMessage` / `DispatchMessage` を別スレッドから呼ばない。
