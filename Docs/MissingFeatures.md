# FBZZ Engine — 未実装・不足機能一覧

最終更新: 2026-05-22  
調査対象ブランチ: `develop`

---

## A. バグ / 不一致

| # | 問題 | 場所 | 詳細 | 状態 |
|---|------|------|------|------|
| A-1 | IRenderer の Create* が shared_ptr を返したまま | `Engine/include/Engine/Renderer/IRenderer.hpp` | ResourceSystem 移行後も `CreateVertexBuffer` 等は `shared_ptr<IBuffer>` 返却。`SetRenderTarget` / `GetImTextureID` だけ Handle 版オーバーロードが存在する混在状態 | **修正済み** — `CreateNative*` として `private` 化し `ResourceManager` を friend に。公開 API は全 Handle ベースに統一 |
| A-2 | AssetBrowserPanel ダブルクリック未実装 | `Editor/src/Panels/AssetBrowserPanel.cpp` | `// ダブルクリックでシーンを開く (.fbzz)` のコメントのみ。SceneSerializer::Load() 呼び出しなし | **修正済み** — 拡張子 `.fbzz` 判定 → `SceneSerializer::Load()` 呼び出し実装 |
| A-3 | 設計書が実装より古い | `Docs/renderer/Design.md` / `Docs/renderer/ResourceSystem.md` | `Material.hpp` / `Mesh.hpp` は既に ResourceHandle ベースだが設計書は `shared_ptr<IBuffer>` と記載。ResourceSystem.md のチェックリストが全て `[ ]` のまま | **修正済み** — `Design.md` の IRenderer / DrawCall 記述を Handle ベースに更新。ResourceSystem.md のチェックリストを全て `[x]` に更新 |

---

## B. 部分実装（骨格はあるが機能が欠けている）

| # | 機能 | 場所 | 欠けている部分 |
|---|------|------|--------------|
| B-1 | showColliders 描画 | `EditorContext` に `bool showColliders = false` が存在 | Viewport の描画ループで `DebugDraw::Box/Sphere` を呼ぶコードが未接続 |
| B-2 | showLightRange 描画 | `EditorContext` に `bool showLightRange = true` が存在 | PointLight / SpotLight の範囲球・コーンを DebugDraw で描く処理が未接続 |
| B-3 | 音声システム | `Engine/src/Audio/XAudio2Device.cpp` | 3D 音声定位・リバーブ等エフェクト・ストリーミング再生が未実装。2D BGM / SE のみ動作 |

---

## C. ロードマップ内の未着手（設計済み、実装なし）

詳細設計は各 `Docs/` ドキュメントを参照。

| # | 機能 | 設計書 | Step |
|---|------|--------|------|
| C-1 | DX12 Renderer | `Docs/shaders/Design.md` | Step 7 |
| C-2 | RenderGraph (DAG ベース) | `Docs/Design.md` | Step 7 |
| C-3 | DXR (Ray Tracing) — Shadow / Reflection | `Docs/shaders/Design.md` | Step 7 |
| C-4 | IBL (Image Based Lighting) | `Docs/shaders/Design.md` — `Rendering/IBL.hlsli` | Step 7 |
| C-5 | Platform/DX12.hlsli (SM 6.5 / Wave Intrinsics マクロ) | `Docs/shaders/Design.md` | Step 7 |
| C-6 | compile_shaders_dx12.bat (dxc.exe ベース) | `Docs/shaders/Design.md` | Step 7 |

---

## D. 設計外の不足機能

### D-1. レンダリング

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-1-1 | スケルタルアニメーション | Bone / BlendShape / AnimationClip / Animator。人・キャラクター表現に必須 | 高 |
| D-1-2 | LOD (Level of Detail) | 距離に応じてメッシュを切り替え。遠景の描画負荷削減 | 中 |
| D-1-3 | GPU Instancing | 同一メッシュを1ドローコールで大量描画（草・木・石など） | 中 |
| D-1-4 | Occlusion Culling | 隠れたオブジェクトを GPU / CPU で除外 | 中 |
| D-1-5 | Terrain レンダリング | 高さマップベースの地形システム | 低 |
| D-1-6 | Decal システム | 弾痕・血痕などをサーフェスに投影 | 低 |
| D-1-7 | SSGI (Screen Space Global Illumination) | SSAO の上位互換。間接照明の近似 | 低 |

### D-2. 物理エンジン

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-2-1 | TriangleMeshCollider | 任意の三角形メッシュとの衝突。地形・建物に必須 | 中 |
| D-2-2 | ConvexHullCollider | 任意形状の凸コライダー（GJK / EPA ベース） | 中 |
| D-2-3 | CCD (Continuous Collision Detection) | 高速オブジェクトが薄い壁をすり抜けない仕組み | 中 |
| D-2-4 | Trigger / Collision イベント通知 | `isTrigger` フラグは存在するが、PhysicsSystem が衝突ペアを Script に通知するパイプラインが未実装。Script 側コールバックは D-9-1 / D-9-2 を参照 | 中 |

### D-3. 数学ライブラリ

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-3-1 | Ray クラス | `origin + t * direction`。現在 MousePicking は ViewportPanel 内に直書きで再利用不可 | 中 |
| D-3-2 | Frustum クラス | 視錐台の 6 平面。Frustum Culling に必要 | 中 |
| D-3-3 | Plane クラス | 平面定義。交差判定・反射ベクトル計算に使用 | 低 |

### D-4. エディター

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-4-1 | Scene Hot Reload | TOML ファイルの変更を検知し自動再読み込み。設計方針「TOML + AI → Hot Reload → Viewport」に明記されているが未実装 | 高 |
| D-4-2 | SceneHierarchyPanel 右クリックメニュー | GameObject 追加 / 削除 / Duplicate。設計書に記載あり。実装状況を要確認 | 中 |
| D-4-3 | Shader 切り替え UI | InspectorPanel > MeshRenderer でシェーダーファイルを差し替える UI | 中 |
| D-4-4 | テクスチャプレビュー | AssetBrowserPanel でサムネイル表示。現状はファイル名テキストのみ | 低 |

### D-5. UI システム

エンジンには現在 2D UI 専用の描画レイヤーが存在しない。ImGui はエディター専用であり、ゲームランタイム UI には使用しない設計とする。

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-5-1 | **UIRenderer** | 2D スプライト・テキスト・矩形を UI 座標系 (スクリーン空間) で描画するレンダラー。`IRenderer` に `SubmitUI(const UIDrawCall&)` を追加するか、独立した `IUIRenderer` インターフェースとして設ける | 高 |
| D-5-2 | **UI Scene / UICanvas** | 3D シーンとは独立した UI 専用シーン (または専用レイヤー)。`UICanvas` コンポーネントがルートとなり、`UIElement` (Image / Text / Button / Layout) をツリーで管理する | 高 |
| D-5-3 | **Play ビューポート — MainCamera + UI 合成** | Play モード / Game ビューポートで「3D シーンを MainCamera で描いた RT」の上に「UI Scene を重ねて合成」して表示する。現状の GameViewport は 3D RT のみ。UIRenderer の出力を Composite パスまたは別パスで重ねる仕組みが必要 | 高 |
| D-5-4 | **UIElement 基底 / ウィジェット** | `UIElement` (位置・サイズ・Anchor・Pivot)、`UIImage` (テクスチャ描画)、`UIText` (フォント / SDF レンダリング)、`UIButton` (Hover / Click イベント) の各コンポーネント | 中 |
| D-5-5 | **UI レイアウトシステム** | Anchor / Pivot / StretchFill などの Unity 相当のレイアウト計算。親の Rect が変わったときに子を自動リサイズ | 中 |
| D-5-6 | **フォントレンダリング** | SDF (Signed Distance Field) フォントまたは FreeType による TrueType ラスタライズ。`UIText` の描画バックエンド | 中 |
| D-5-7 | **UI 入力イベント** | マウス座標を UI 座標系に変換し、`UIButton` / `UISlider` 等のヒットテストと OnClick / OnHover コールバックを発火する | 中 |
| D-5-8 | **InspectorPanel — UIElement 編集** | エディターの Inspector で UIElement / UIImage / UIText のパラメータを編集できるようにする | 低 |
| D-5-9 | **SceneSerializer — UI Scene 対応** | `.fbzz` ファイルに UI Scene (UICanvas ツリー) をシリアライズ / デシリアライズする | 低 |

### D-6. エンジン基盤

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-6-1 | マルチスレッド / Job System | ゲームループが完全シングルスレッド。Step 7 以降に明示的に先送り済み | 高 (Step 7) |
| D-6-2 | Input アクションマップ | 現状は `Input::IsKeyDown(KeyCode)` 直叩きのみ。コントローラー対応・リバインド不可 | 中 |
| D-6-3 | GamePad / Controller 入力 | XInput 等。現状キーボード・マウスのみ | 中 |
| D-6-4 | Asset 非同期ロード | `AssetManager::Load<T>()` が同期のみ。大アセット読み込み中にゲームループが停止する | 中 |
| D-6-5 | カスタムメモリアロケーター | 全て `std::allocator` 任せ。メモリ使用量トラッキングなし | 低 |
| D-6-6 | GPU プロファイラー統合 | StatusBar に FPS のみ。GPU タイムスタンプ・各パス CPU/GPU 時間の内訳なし | 低 |

### D-7. DebugDraw 拡張

現状の `DebugDraw` は `Line` / `Box` (AABB のみ) / `Sphere` / `Capsule` の 4 関数のみ。最大頂点数 4096 固定・DepthTest 常時 OFF・回転パラメータなし。B-1 / B-2 のコライダー・ライト範囲描画を接続する前に拡張が必要。

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-7-1 | OBB 描画（回転付き Box） | `Box(center, halfExtents, rotation: Quaternion, color)` のオーバーロード追加。コライダー・オブジェクトの向きを正しく表示するために必須 | 高 |
| D-7-2 | Arrow（方向ベクトル） | `Arrow(origin, direction, length, color)` — velocity・force・法線の可視化。Line + 小コーンで実装 | 中 |
| D-7-3 | Axes（座標軸） | `Axes(origin, rotation, size)` — Transform の軸を RGB 3 本の Arrow で描画。現状は呼び出し側が毎回 `Line` 3 本を手書き | 中 |
| D-7-4 | Cone（コーン） | `Cone(apex, direction, halfAngle, height, color)` — SpotLight の照射範囲可視化（B-2）に必要 | 中 |
| D-7-5 | 深度テスト切り替え | `DepthMode` パラメータ追加（`DEPTH_OFF` / `DEPTH_READ`）。オブジェクト裏側のコライダーをオクルージョン付きで表示するか選択できる | 低 |
| D-7-6 | 持続時間（duration） | `duration` 秒間だけ残る描画。毎フレーム呼ばなくても一定時間表示し続けられる（衝突点・イベント可視化） | 低 |
| D-7-7 | 最大頂点数の動的拡張 | 現状 4096 頂点ハードリミット。シーン規模が増えると超過して描画が欠ける。動的に頂点バッファを確保するか上限を引き上げる | 低 |

### D-9. スクリプトシステム

骨格（`Script` 基底クラス・`ScriptFactory`・`ScriptSystem`・`IReflector`・Inspector 統合・TOML シリアライズ）は実装済み。以下は未実装の拡張機能。

| # | 機能 | 概要 | 優先度 |
|---|------|------|--------|
| D-9-1 | OnCollisionEnter / Stay / Exit | 衝突検出は `PhysicsSystem` で動作しているが、Script へのイベント通知パイプラインが未実装。`World::Step` 後の接触ペアを取得し、前フレームとの差分から Enter / Stay / Exit を判定して該当 Script のメソッドを呼ぶ仕組みが必要 | 高 |
| D-9-2 | OnTriggerEnter / Stay / Exit | `isTrigger` フラグは存在（D-2-4 参照）するが、Script 基底クラスへのコールバックが未定義。D-9-1 と同じ通知パイプラインで実現可能 | 高 |
| D-9-3 | OnDestroy() ライフサイクル | Script 基底クラスに `OnDestroy` がない。`FlushDestroyQueue` 実行前に Script の後処理（イベント購読解除・ネイティブリソース解放など）を呼ぶ手段が存在しない | 中 |
| D-9-4 | IReflector — enum / ResourceHandle 型対応 | 現状の `IReflector::Field` は `float` / `int` / `bool` / `Vector3` / `Vector4` / `Quaternion` / `string` の 7 型のみ。`enum class` や `ResourceHandle<T>`（テクスチャ・メッシュ参照）をフィールドとして宣言・Inspector 編集・TOML 保存できない | 中 |
| D-9-5 | 1 GameObject に複数 Script アタッチ | `ComponentArray` の制約上、`ScriptComponent` は 1 エンティティにつき 1 つまで。Unity 相当の複数スクリプトが必要な場合は内部で `Composite Script` パターンを使うか、ECS 側で複数コンポーネント対応が必要 | 低 |
| D-9-6 | Script ホットリロード | スクリプトは C++ ネイティブのため再コンパイルなしに変更不可。DLL 差し替えや Lua / Python バインディング、あるいは C++ ホットリロードライブラリ (cr.h 等) の導入が選択肢。Step 7 以降 | 低 (Step 7 以降) |

---

## 優先度まとめ

| 優先度 | 項目 |
|--------|------|
| 今すぐ | — (A-1〜A-3 はすべて解消済み) |
| 近い将来 | B-1, B-2, D-4-1, D-4-2, D-5-1, D-5-2, D-5-3, D-7-1 (OBB), D-9-1, D-9-2 |
| Step 7 | C-1 〜 C-6, D-6-1, D-9-6 |
| 余裕があれば | D-1-1 〜 D-4-4, D-5-4 〜 D-5-9, D-6-2 〜 D-6-6, D-7-2 〜 D-7-7, D-9-3 〜 D-9-5 |
