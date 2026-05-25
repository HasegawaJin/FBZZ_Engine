# FBZZ Engine

C++20 で自作する 3D ゲームエンジン。数学・物理エンジンをゼロから実装し、DirectX 11 レンダラーを抽象インターフェースで隠蔽する構成。

---

## 特徴

- **数学ライブラリ自作** — Vector2 / Vector3 / Vector4 / Matrix3 / Matrix4 / Quaternion / Ray / Frustum / Plane を GLM に頼らず実装
- **物理エンジン自作** — GJK + EPA による凸形状衝突検出、BVH ブロードフェーズ、インパルスベース衝突解決、CCD、各種コンストレイント (Distance / Hinge / Spring / Rope / Chain)
- **抽象レンダラー** — `IRenderer` インターフェースで DX11 を隠蔽。将来 DX12 / レイトレに差し替え可
- **PBR + 遅延レンダリング** — GBuffer パス・遅延ライティング・シャドウマップ・SSAO・Bloom・FXAA を実装
- **スケルタルアニメーション** — FBX からボーン・ウェイトをインポートし GPU スキニングでアニメーション再生
- **ImGui エディター** — Hierarchy / Inspector / AssetBrowser / Viewport / Console / StatusBar を備えたエディター
- **シーン永続化** — TOML ベースの `.fbzz` フォーマットでシーンを保存・読み込み
- **Win32 ネイティブ** — GLFW に依存せず Win32 API でウィンドウ・入力を管理
- **C++20** — `std::span`, Concepts, designated initializers を活用

---

## 実装済みシステム一覧

### コアエンジン

| システム | 概要 |
|---------|------|
| `Application` | メインループ・固定 Update (物理) + 可変 Update (ゲーム) + Render |
| `Window` | Win32 API ウィンドウ生成・リサイズ・フルスクリーン対応 |
| `Time` | `deltaTime` / `fixedDeltaTime` / `timeScale` 管理 |
| `Logger` | ログレベル付きログ、`ILogSink` で Console パネルへ転送 |
| `Input` | キーボード・マウスの状態管理 (`GetKey` / `GetKeyDown` / `GetKeyUp`) |
| `EventBus` | 型安全なイベントバス |

### シーン管理 (ECS 風 SoA)

| クラス | 概要 |
|--------|------|
| `Scene` | エンティティ管理・システム実行 |
| `GameObject` | エンティティのラッパー。`AddComponent<T>` / `GetComponent<T>` |
| `Transform` | 位置・回転・スケール。親子階層・ワールド行列キャッシュ |
| `SceneManager` | シーン遷移・ロード管理 |
| `SceneSerializer` | TOML (.fbzz) 形式でシーンを保存・読み込み |
| `Script` / `ScriptComponent` | ユーザー定義スクリプトの `OnStart` / `OnUpdate` |

### コンポーネント

| コンポーネント | 概要 |
|--------------|------|
| `MeshRenderer` | スタティックメッシュの描画 |
| `SkinnedMeshRenderer` | スキンドメッシュ + GPU スキニング描画 |
| `AnimatorComponent` | `AnimationClip` の再生・ブレンド |
| `CameraComponent` | カメラ FOV・Near/Far 管理 |
| `LightComponent` | Directional / Point / Spot ライト |
| `ColliderComponent` | AABB / Sphere / Capsule / ConvexHull / TriangleMesh コライダー |
| `RigidBodyComponent` | 物理剛体アタッチ |
| `AudioSourceComponent` | XAudio2 サウンド再生 |
| `MaterialComponent` | マテリアル参照 |
| `ParticleEmitter` | パーティクルシステム |
| `SkyRenderer` | スカイボックス描画 |
| `VolumeComponent` | ポストプロセスボリューム |
| `UICanvas` / `UIImage` / `UIText` / `UIButton` / `UILayoutGroup` | UI コンポーネント群 |

### システム (Scene 内で毎フレーム実行)

| システム | 概要 |
|---------|------|
| `TransformSystem` | 親子階層のワールド行列を更新 |
| `PhysicsSystem` | 固定ステップ物理シミュレーション |
| `AnimatorSystem` | アニメーションクリップを進行し骨格行列を更新 |
| `RenderSystem` | スタティック・スキンドメッシュの描画コール生成 |
| `AudioSystem` | 3D サウンド更新 |
| `UISystem` | UI レイアウト計算・描画 |
| `ScriptSystem` | ユーザースクリプトの Update 呼び出し |
| `ColliderDebugDrawSystem` | コライダーワイヤーフレームをデバッグ描画 |
| `AnimatorDebugDrawSystem` | ボーン姿勢をデバッグ描画 |

### レンダラー (DX11 実装)

| 機能 | 概要 |
|------|------|
| GBuffer パス | Albedo / Normal / Roughness / Metallic / Emissive |
| 遅延ライティング | Directional / Point / Spot ライトを GBuffer から解決 |
| シャドウマップ | Directional / Spot ライトの深度マップ |
| SSAO | コンピュートシェーダーによるスクリーンスペース AO |
| Bloom | Downsample / Upsample コンピュートシェーダー |
| FXAA | ファストアンチエイリアシング |
| DebugDraw | 線・矩形・球・カプセルのワイヤーフレーム即時描画 |

### シェーダー (HLSL)

| カテゴリ | ファイル |
|---------|---------|
| マテリアル | `PBR.hlsl` / `SkinnedPBR.hlsl` / `BlinnPhong.hlsl` / `Toon.hlsl` / `Unlit.hlsl` / `Particle.hlsl` |
| パイプライン | `GBuffer.hlsl` / `DeferredLighting.hlsl` / `ShadowMap.hlsl` / `SkinnedShadowMap.hlsl` |
| ポストプロセス | `SSAO.cs.hlsl` / `SSAOBlur.cs.hlsl` / `BloomDownsample.cs.hlsl` / `BloomUpsample.cs.hlsl` / `FXAA.hlsl` / `Composite.hlsl` |

### 物理エンジン

| 機能 | 概要 |
|------|------|
| 衝突形状 | AABB / Sphere / Capsule / ConvexHull / TriangleMesh |
| ブロードフェーズ | BVH (Bounding Volume Hierarchy) |
| ナローフェーズ | GJK + EPA |
| CCD | 連続衝突検出 |
| コンストレイント | Distance / Hinge / Spring / Rope / Chain |
| ソルバー | インパルスベース。半陰的オイラー積分 |

### スケルタルアニメーション

| 機能 | 概要 |
|------|------|
| `Skeleton` | ボーン階層データ |
| `AnimationClip` | キーフレーム列 (位置・回転・スケール) |
| `ModelImporter` | Assimp 経由で FBX からスケルタルデータをインポート |
| GPU スキニング | `SkinnedPBR.hlsl` でボーン行列を GPU 適用 |

### アセット管理

| 機能 | 概要 |
|------|------|
| `AssetManager` | モデル・テクスチャ・マテリアル・アニメーションのキャッシュ管理 |
| `ResourceManager` | GPU リソース (`IBuffer` / `ITexture` / `IShader`) のプール管理 |

### エディター (ImGui)

| パネル | 概要 |
|--------|------|
| Hierarchy | シーン内の GameObject ツリー表示・選択・作成・削除 |
| Inspector | 選択オブジェクトのコンポーネント編集 (全コンポーネント対応) |
| AssetBrowser | プロジェクトアセットのファイルブラウザ。FBX サブアセット (Skeleton / AnimationClip) アイコン表示 |
| Viewport | ゲームビュー描画・Gizmo 操作 |
| Console | `Logger` のログ出力表示 |
| StatusBar | 再生・停止・Gizmo 切り替えツールバー |
| PlayModeController | Play / Stop によるシーン状態のスナップショット復元 |

### オーディオ

| 機能 | 概要 |
|------|------|
| `XAudio2Device` | `IAudioDevice` 実装。XAudio2 で 3D サウンド再生 |

---

## モジュール構成

```
FBZZ_Engine
├── Projects/
│   ├── Math/      独立ライブラリ。自作数学ライブラリ (GLM 不使用)
│   ├── Physics/   独立ライブラリ。Math をリンク (Bullet / PhysX 不使用)
│   ├── Engine/    コアエンジン。Physics / Math をリンク
│   ├── Editor/    ImGui エディター。Engine をリンク
│   └── Sandbox/   動作確認・サンプルアプリ
├── Assets/
│   ├── shaders/   HLSL シェーダー群
│   └── scenes/    シリアライズ済みシーン (.fbzz)
└── Docs/          設計ドキュメント・規約
```

依存方向: `Sandbox / Editor → Engine → Physics → Math`

---

## ビルド方法

### 要件

- Windows 10/11
- Visual Studio 2022 以降 (C++20 対応)
- CMake 3.20 以上
- Windows SDK (DirectX 11 同梱)
- Assimp (FBX インポート)

### ビルド手順

```powershell
git clone https://github.com/Ungoi18/FBZZ_Engine.git
cd FBZZ_Engine
cmake -B build -G "Visual Studio 17 2022" -A x64
```

Visual Studio で `FBZZ_Engine.sln` を開き `Ctrl+Shift+B` でビルド。

シェーダーコンパイルは `Assets/shaders/compile_shaders.bat` を実行。

---

## 使用ライブラリ

| ライブラリ | 用途 |
|-----------|------|
| DirectX 11 SDK | レンダリング API |
| Microsoft::WRL (ComPtr) | COM リソース RAII |
| XAudio2 | 3D オーディオ |
| Assimp | FBX / OBJ メッシュ・スケルタルデータ読み込み |
| ImGui | エディター UI |
| toml++ | シーンシリアライゼーション |

**使用しないライブラリ:** GLM (数学は自作) / GLFW (ウィンドウは Win32) / Bullet・PhysX (物理は自作)

---

## ライセンス

MIT License — 詳細は [LICENSE](LICENSE) を参照。
