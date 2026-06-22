# Asset & Script Design

---

## Asset システム

### 概要

AssetManager はスロットプール + 世代カウンタ方式でアセットを管理する。
ポインタの代わりに軽量な `AssetHandle<T>` を使って参照することで、
アセットの再ロード・削除後の dangling reference を世代不一致で検出する。

```
AssetHandle<T>
  id  : uint32   ← スロットインデックス (0 = null)
  gen : uint32   ← 世代カウンタ (スロット再利用で世代が上がる)

AssetManager::Get(handle) → 世代不一致なら nullptr を返す
```

### 管理アセット種別

| 型 | 説明 |
|----|------|
| `ModelAsset` | メッシュ + マテリアル参照のコンテナ |
| `TextureAsset` | GPU テクスチャ + ミップマップ情報 |
| `MaterialAsset` | シェーダー種別 + テクスチャスロット + パラメーター |
| `AnimationClip` | ボーントラック + キーフレーム配列 |
| `AnimatorControllerAsset` | ステートマシン + トランジション条件 |
| `TerrainAsset` | 高さマップ + マルチレイヤーペイント + グリッド分割情報 |

---

## バイナリアセット形式 (fz* フォーマット)

Editor でインポートされた外部ファイル (FBX / glTF / PNG 等) は
エンジン専用バイナリ形式に変換して `Assets/` に保存される。

### 設計方針

- **フラット配列のみ** — ポインタ・可変長フィールドを含まない
- **ゼロコピー読み込み可能** — ヘッダー後に連続する配列を `memcpy` またはメモリマップで直接使用
- **magic + version** — ファイル種別を先頭 4 バイトのマジックで識別し、version 不一致は即エラー
- **static_assert でサイズ固定** — フォーマット変更の見逃しをコンパイル時に検出

### FzMesh (`FZMH`)

```
FzMeshHeader (36 bytes)
  magic[4]      = "FZMH"
  version       = 1
  flags         = FZMESH_FLAG_SKINNED (ビット0)
  vertexCount
  indexCount
  boundsCenter[3], boundsRadius

[Vertex 配列]    ← flags で静的 Vertex / SkinnedVertex を切替
[uint32 インデックス配列]
```

### FzSkeleton (`FZSK`)

```
FzSkelHeader (84 bytes)
  magic[4] = "FZSK", version, rootNodeIndex
  nodeCount, boneCount
  rootInverse[16]  ← row-major Matrix4

FzSkeletonNodeData × nodeCount (各 180 bytes + 可変 children)
  name[64], parentIndex, boneIndex
  bindTranslation/Rotation/Scale, localBindTransform[16]
  childCount → 直後に childCount × int32_t

FzBoneData × boneCount (各 136 bytes)
  name[64], nodeIndex, offsetMatrix[16]
```

### FzAnimation (`FZAN`)

```
FzAnimHeader (160 bytes)
  magic[4] = "FZAN", version
  name[128], durationTicks, ticksPerSecond, trackCount

FzAnimTrackHeader × trackCount (各 144 bytes)
  nodeName[128]
  positionCount, rotationCount, scaleCount

  → FzVectorKey    × positionCount  (各 24 bytes: time + xyz)
  → FzQuaternionKey × rotationCount (各 24 bytes: time + xyzw)
  → FzVectorKey    × scaleCount
```

---

## Import パイプライン

```
外部ファイル (FBX / glTF / PNG / WAV)
  │
  ├─ IAssetImporter (種別ごとに実装)
  │    ModelAssetImporter  ← Assimp でパース → FzMesh / FzSkel / FzAnim に変換
  │    ImageImporter       ← stb_image でデコード → DX11 テクスチャ生成
  │    AnimCtrlImporter    ← JSON → AnimatorControllerAsset
  │    TerrainImporter     ← FzTerrain バイナリ → TerrainAsset
  │
  └─ AssetManager::Load() → AssetHandle<T> を返す
```

`IAssetImporter` インターフェースを実装することで新形式を追加できる。
エディターの `AssetBrowser` がファイル変更を監視し、変更検知時に自動再インポートを行う。

---

## Script システム

### アーキテクチャ

スクリプトは **独立した DLL** としてコンパイルされ、実行時にホットロードされる。
Engine 実装型へのダイレクト依存を排除するため、スクリプトは `ScriptProxy` 経由でのみコンポーネントにアクセスする。

```
Assets/Scripts/PlayerScript.hpp  ← ユーザー記述
    ↓ ScriptCodeGen (コード生成)
Assets/Scripts/PlayerScript.generated.hpp

    ↓ MSBuild → Scripts.dll

ScriptDllLoader
  ├─ ABI 署名検証
  ├─ RegisterComponents() で型を ComponentRegistry に登録
  └─ Scene::Instantiate() がファクトリー経由でスクリプトを生成
```

### ScriptProxy 一覧

スクリプトは以下のプロキシを通じて Engine 機能にアクセスする。
DLL 境界を越えて安全な型付きアクセスを提供する。

| プロキシ | 提供機能 |
|---------|---------|
| `ScriptTransformProxy` | 座標・回転・スケール操作 |
| `ScriptPhysicsProxy` | Raycast / AddForce / コライダー参照 |
| `ScriptInputProxy` | キー・マウス・ゲームパッド入力 |
| `ScriptAnimatorProxy` | ステート遷移・パラメーター操作 |
| `ScriptMaterialProxy` | マテリアルパラメーターの動的変更 |
| `ScriptCameraProxy` | FOV / クリッピング / メインカメラ操作 |
| `ScriptLightProxy` | 光源色・強度・範囲の動的変更 |
| `ScriptAudioProxy` | BGM / SE の再生・停止・音量制御 |
| `ScriptNavigationProxy` | NavMesh 経路探索・エージェント操作 |
| `ScriptParticleProxy` | パーティクルエミッター制御 |
| `ScriptTrailProxy` / `ScriptMeshTrailProxy` | トレイルエフェクト制御 |
| `ScriptUIProxy` | UI Canvas / Text / Button の動的操作 |
| `ScriptPostProcessProxy` | ポストプロセスパラメーターの実行時変更 |
| `ScriptSceneProxy` | シーン遷移・GameObject 生成・破棄 |
| `ScriptDebugProxy` | DebugDraw / ログ出力 |
| `GizmoProxy` | エディター Gizmo 描画 |
| `ScriptMemoryProxy` | フレームアロケーターへのアクセス |

### ABI 安全性

DLL とホスト実行ファイルが共有する型のレイアウトを **FNV-1a ハッシュで署名化** し、
古い DLL のロードを防ぐ。

```cpp
// GetScriptDllAbiSignature() が mix する値
sizeof(Scene), alignof(Scene)
sizeof(Script), alignof(Script)
sizeof(ScriptComponent), alignof(ScriptComponent)
std::tuple_size_v<ComponentList>  ← コンポーネント数
_MSC_VER                          ← コンパイラバージョン
_ITERATOR_DEBUG_LEVEL             ← Debug/Release の差異
```

Engine か Script のどちらかだけを再ビルドした場合、署名が不一致となり
ロードを拒否してアクセス違反を未然に防ぐ。

### スクリプトライフサイクル

```cpp
class PlayerScript : public Script {
public:
    void OnStart()           override;   // GameObject がアクティブになった初フレーム
    void OnUpdate(float dt)  override;   // 毎フレーム更新
    void OnLateUpdate(float dt) override; // Animator / IK 後の後処理
    void OnFixedUpdate(float dt) override; // 固定タイムステップ物理
    void OnDestroy()         override;   // 破棄時クリーンアップ

    // 衝突イベント
    void OnCollisionEnter(const CollisionInfo& info) override;
    void OnCollisionExit(const CollisionInfo& info)  override;
    void OnTriggerEnter(const CollisionInfo& info)   override;
    void OnTriggerExit(const CollisionInfo& info)    override;
};
```

### ホットリロードフロー

```
1. Assets/Scripts/ 以下のファイル変更を AssetFileWatcher が検知
2. ScriptDllLoader::Unload() → 現在の DLL をアンロード
3. Compiler::Build() → MSBuild で Scripts.dll を再コンパイル
4. ScriptDllLoader::Load() → 新 DLL をロード
5. ABI 署名検証 → 不一致なら警告してロールバック
6. RegisterComponents() → 型を ComponentRegistry に再登録
7. Scene 上の既存 ScriptComponent をマイグレーション
       (シリアライズ済みプロパティを新インスタンスに移植)
```

エディター Play Mode 中のホットリロードに対応し、
スクリプト変更を止めずにゲームループに反映できる。
