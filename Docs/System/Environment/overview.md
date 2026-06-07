# Environment / Gameplay コンポーネント 設計 — 全体概要

## 対象コンポーネント
`TerrainComponent` / `WaterComponent` / `VolumeComponent` / `AudioSourceComponent` / `CharacterControllerComponent`
システム: `TerrainRenderSystem` / `WaterRenderSystem` / `AudioSystem`

---

## 実装済み機能

### TerrainComponent / TerrainRenderSystem

| 機能 | 状態 |
|---|---|
| float ハイトマップ（row-major、[-1,1] 正規化）+ maxHeight | ✅ |
| RGBA8 × 4ch スプラットマップ（最大 4 レイヤーブレンド） | ✅ |
| `GetHeightAt()` / `ComputeNormal()` 高さクエリ API | ✅ |
| `InitFlat()` でゼロ初期化 | ✅ |
| heightDirty / splatDirty / colliderDirty 3 本ダーティフラグ | ✅ |
| 外部アセット分離（`terrainAssetPath`） | ✅ |
| チャンク分割描画 + 3 段階 LOD + フラスタムカリング | ✅ |
| splatmap(t0) + diffuse/normal/AO/Roughness ×4(t1-12) フルバインド | ✅ |
| Directional / Point×8 / Spot×4 ライティング | ✅ |
| Unlit / Wireframe モード対応 | ✅ |
| Reflect によるシリアライズ | ✅ |

### WaterComponent / WaterRenderSystem

| 機能 | 状態 |
|---|---|
| Gerstner 波 × 4 本（direction / amplitude / wavelength / steepness） | ✅ |
| CPU 水面高さクエリ `GetSurfaceHeightAt()` | ✅ |
| チャンク分割描画 + フラスタムカリング | ✅ |
| 岸辺フォームマスク（Terrain 高さ差から CPU 生成） | ✅ |
| 動的波紋（`AddWaterRipple()`、リングパターン CPU テクスチャ） | ✅ |
| 水しぶきパーティクル（`QueueWaterSplash()` → ParticleEmitter GO 動的生成） | ✅ |
| normalMap×2 / foamTex / foamMask / envTex / sceneColor / flowMap / rippleTex 9 スロット | ✅ |
| Unlit / Wireframe モード対応 | ✅ |

### VolumeComponent

| 機能 | 状態 |
|---|---|
| VolumeType 列挙（重力 / 磁場 / 渦 / 内向き / 浮力 / 爆発 等） | ✅ |
| gravity / magneticField / swirlStrength / buoyancy / drag 等パラメーター | ✅ |
| timeScale（ボリューム内時間スケール） | ✅ |
| duration（負値=無限）+ elapsed で有効期間管理 | ✅ |

### AudioSourceComponent / AudioSystem

| 機能 | 状態 |
|---|---|
| `clipPath` + `playOnAwake` / `loop` / `volume` | ✅ |
| `m_played` フラグによる初回再生重複防止 | ✅ |
| ScriptProxy: Play / Stop / Pause / SetVolume / SetLoop | ✅ |

### CharacterControllerComponent

| 機能 | 状態 |
|---|---|
| Grounded / Falling / IntentionalJump 3 状態ステートマシン | ✅ |
| 接地判定（法線 Y + groundContactGrace タイマー） | ✅ |
| jumpGroundIgnoreTime（着地直後ジャンプの誤着地防止） | ✅ |
| ledgeFallThreshold（崖落ち検出） | ✅ |
| StabilizeGroundedVelocity / RemoveVelocityIntoGround | ✅ |
| Animator 連携（verticalSpeed 公開） | ✅ |
| 全チューニングパラメーター Reflect 対応 | ✅ |

---

## 不足機能・バグ

---

### TerrainComponent

#### 重要度：高

**`colliderDirty` が `Reflect()` に含まれていない**

初期値 `true` で自動コライダー構築を意図しているが、シーンロード後に `colliderDirty` が復元されないためコライダー再構築がトリガーされない可能性がある。

**修正方針：** `Reflect()` に追加するか、SceneSerializer でロード後に `colliderDirty = true` を保証する。

---

**チャンクキャッシュの Entity 削除時クリーンアップ未実装**

`static` な `s_chunkCache` / `s_texCache` はエンティティ削除後も解放されず、シーン遷移やエンティティ破棄時にメモリリークが発生する。

**修正方針：** エンティティ無効時のキャッシュ削除ロジックを `TerrainRenderSystem` に追加する。

---

**EntityID 逆引きが O(N) 線形探索**

`for (EntityID candidate : scene.GetEntities<TerrainComponent>())` によるポインタ比較は Terrain 数に線形比例する。

**修正方針：** ECS への EntityID 付き View またはコンポーネントへの逆引きポインタを利用する。

---

#### 重要度：中

**LOD 境界距離がハードコード**

`chunkWorldSize * 2.0f / 6.0f` が固定でシーン・地形サイズに最適値が異なる。

**修正方針：** `RenderSettings` または TerrainComponent に LOD 距離パラメーターを公開する。

---

**`heightData` サイズ検証が `assert` のみ**

Release ビルドで検証が消える。

**修正方針：** 実行時チェックに変更するか `InitFlat` 後に自動調整する防御コードを追加する。

---

#### 重要度：低

**境界頂点のタンジェント計算でスパンが半分になる**

コーナー頂点で `txL = max(x-1, 0)` のクランプにより span が通常の半分になりタンジェントが不正確になる。

**修正方針：** 境界判定を分岐して修正するか、中心差分を常に ±1 で計算するよう統一する。

---

### WaterComponent

#### 重要度：高

**深度テクスチャが常に無効**

```cpp
const renderer::ResourceHandle<renderer::TextureTag> depthTex = {};  // 意図的に空
```

「DX11 で RTV/DSV と同時 SRV バインド禁止」のため削除されているが、深度参照なしでは水中屈折・shallow/deepColor の距離フェードが正しく計算できない。

**修正方針：** Depth Copy パスを追加してから深度テクスチャを正式対応する。

---

**フォームマスクが静的（`foamDirty` 依存）**

水面 Transform や Terrain の `heightData` が実行中に変化しても、`foamDirty` を立てない限りフォームマスクが更新されない。

**修正方針：** Terrain の `heightDirty` と Water の `foamDirty` を連動させる仕組みを追加する。

---

**メッシュ・テクスチャ・波紋キャッシュの Entity 削除時クリーンアップ未実装**

Terrain と同様。static な `s_meshCache / s_texCache / s_rippleStates` がエンティティ破棄後も残りメモリリークになる。

**修正方針：** エンティティ無効時のキャッシュ削除ロジックを `WaterRenderSystem` に追加する。

---

#### 重要度：中

**波紋テクスチャを毎フレーム Release → 再生成**

`UpdateRippleState` が毎フレーム `Release → CreateTexture` を実行し、CPU/GPU 双方でアロケーション・デアロケーションが繰り返される。

**修正方針：** 128×128 の動的更新テクスチャ（`Map/Unmap`）に変更する。

---

**水しぶきに `std::rand()` を使用**

スレッドセーフでなくシードが固定。

**修正方針：** `<random>` による局所的乱数生成器に切り替える（`Util/Random.hpp` が既存）。

---

**`steepness > 1.0f` の assert のみで事前クランプがない**

Gerstner 波の steepness が 1 を超えると波面が自己交差するが、Release ビルドで assert が消えて頂点が反転する。

**修正方針：** `std::min(wave.steepness, 1.0f)` による防御的クランプを計算前に追加する。

---

#### 重要度：低

**`GerstnerWave` パラメーターが `Reflect()` に含まれていない**

`waves` 配列が Inspector からもシリアライズからも編集できない。

**修正方針：** SceneSerializer または IReflector 側で `waves` の読み書きを実装する。

---

### VolumeComponent

#### 重要度：高

**`type` フィールドが `Reflect()` に含まれていない**

Inspector から `VolumeType` が変更できない。ボリューム種別がエディターから設定不可という重大な欠落。

**修正方針：** IReflector に enum 対応を追加するか、int 経由で `type` を `Reflect` に追加する。

---

**`elapsed` が `Reflect()` に含まれていない**

シーン保存→ロード後にタイマーがリセットされ、Play 中のシーン保存で状態が失われる。

**修正方針：** `elapsed` を `Reflect` に追加するか、ロード後に明示的に `elapsed = 0` を保証する設計方針を文書化する。

---

#### 重要度：中

**ボリューム形状の定義がない**

`VolumeComponent` にコライダー形状（Box/Sphere/Capsule）やサイズパラメーターが存在せず、どのオブジェクトが範囲内に入るかを判定するためのサイズ情報がない。`ColliderVolume` との関係が曖昧。

**修正方針：** `VolumeComponent` に形状サイズフィールドを追加するか、`ColliderComponent` の `isTrigger` と組み合わせる設計を明文化する。

---

#### 重要度：低

| 項目 | 説明 | 修正方針 |
|---|---|---|
| **`duration == 0.0f` の挙動が未定義** | 即時無効化か 1 フレームのみ有効かが未規定 | ゼロガード処理または仕様コメントを追加 |
| **複数タイプのパラメーターが単一 struct に共存** | 異なるタイプで無関係なフィールドが多くメモリ効率が低い | `std::variant` またはサブ構造体化（将来対応） |

---

### AudioSourceComponent

#### 重要度：高

**`AudioSystem` が Stop / Pause / Volume 変更を処理していない**

`AudioSystem.cpp` は `PlayOnAwake` の初回発火のみ実装されており、`ScriptAudioProxy::Stop() / Pause() / SetVolume()` に対応するシステム側コードがない。

**修正方針：** `AudioSourceComponent` に `m_isPlaying / m_isPaused / m_pendingStop` フラグを追加し、`AudioSystem` がこれを毎フレーム見て AudioManager に指示を出す。

---

**`volume` が AudioManager 呼び出し時に渡されていない**

`AudioSystem.cpp` の `PlayBGM / PlaySE` 呼び出しに `asc.volume` が渡されておらず、設定したボリュームが実際の再生に反映されない。

**修正方針：** `AudioManager::PlayBGM / PlaySE` の引数に `volume` を追加し、`AudioSystem` で渡す。

---

**再生中フラグが `m_played`（初回フラグ）のみで状態追跡できない**

Stop / Pause 後の状態が Component で追跡できない。

**修正方針：** `bool m_isPlaying = false` / `bool m_isPaused = false` を追加し、`AudioSystem` が同期する。

---

#### 重要度：中

**3D 空間オーディオが未実装**

ヘッダーコメントに「Transform 連動の 3D 音響はまだ持たせない」と明記されており、位置連動の減衰・パンニングが保留中。

---

**`AudioSystem` の `dt` 引数が未使用**

将来の Fade-in/out 処理の予約と考えられるが、コンパイル警告の原因になりうる。

**修正方針：** `[[maybe_unused]]` を付けるか、フェード処理を実装して使用する。

---

#### 重要度：低

| 項目 | 説明 | 修正方針 |
|---|---|---|
| **`clipPath` の検証がない** | 不正パス時の挙動が AudioManager 任せ | 空文字・拡張子チェックを AudioSystem 側に追加 |
| **`m_played` が `Reflect()` に含まれていない** | シーンロード後に PlayOnAwake が再度発火する | 意図的かどうかを設計判断し、保存が必要なら追加 |

---

### CharacterControllerComponent

#### 重要度：高

**`Jump()` の呼び出しと `ApplyImpulse()` の順序が API で保証されていない**

コメントに「rb->ApplyImpulse は呼び出し元で行う」とあるが、Script 側が順序を誤ると `jumpGroundIgnoreTime` のタイミングがずれてジャンプ直後に着地判定が入る。

**修正方針：** `Jump(RigidBody* rb)` シグネチャでインパルスも内部で適用するか、API ドキュメントで呼び出し順を明示する。

---

#### 重要度：中

**スロープ移動で上り坂接地中にずり落ちる可能性**

`StabilizeGroundedVelocity()` は Y 速度を完全にゼロにするが、上り坂では Y 成分が必要で、斜面角度が `groundedVelSnap` 閾値を超えない場合にずり落ちる可能性がある。

**修正方針：** 斜面法線と移動方向の内積を使って、上り坂では Y 速度の一部を保持する処理を追加する。

---

**`VolumeComponent` の重力変化との連携なし**

重力変化ボリューム内では `fallVelThreshold` などの絶対値閾値が機能しなくなる可能性がある。

**修正方針：** `PhysicsSystem` から effective gravity を取得して閾値を動的スケーリングするか、Volume 内では接地判定を無効化するオプションを追加する。

---

#### 重要度：低

**`UpdateGrounding()` の一部パスで接触なしで着地状態になりえる**

`fabsf(vy) < groundVelThreshold` のみで着地を判定するパスで、空中で減速した場合（上向き Volume 内等）に誤着地する可能性がある。

**修正方針：** 条件に `m_hasGroundContact` を追加するか、設計意図をコメントで補足する。

---

## 修正優先順位まとめ

| 優先度 | コンポーネント | 項目 | 工数目安 |
|---|---|---|---|
| 1 | VolumeComponent | `type` を `Reflect()` に追加 | 小 |
| 2 | AudioSourceComponent | Stop/Pause/Volume を AudioSystem で処理 | 中 |
| 3 | AudioSourceComponent | `volume` を PlayBGM/PlaySE に渡す | 小 |
| 4 | TerrainComponent | チャンクキャッシュのクリーンアップ | 小〜中 |
| 5 | WaterComponent | 波紋テクスチャを Map/Unmap に変更 | 小 |
| 6 | WaterComponent | メッシュ・テクスチャキャッシュのクリーンアップ | 小〜中 |
| 7 | WaterComponent | Depth Copy パス → 深度テクスチャ対応 | 大 |
| 8 | CharacterControllerComponent | Jump() API の整理 | 小 |
| 9 | TerrainComponent | `colliderDirty` のシリアライズ対応 | 小 |
| 10 | WaterComponent | GerstnerWave を Reflect に追加 | 中 |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/Scene/
    Components/
      TerrainComponent.hpp / WaterComponent.hpp
      VolumeComponent.hpp / AudioSourceComponent.hpp
      CharacterControllerComponent.hpp
    ScriptProxy/
      ScriptAudioProxy.hpp
    Systems/
      TerrainRenderSystem.hpp / WaterRenderSystem.hpp / AudioSystem.hpp
  src/Scene/Systems/
    TerrainRenderSystem.cpp / WaterRenderSystem.cpp / AudioSystem.cpp

Docs/System/Environment/
  overview.md    ← このファイル
```

---

## 隣接ドキュメント

- [../Physics/overview.md](../Physics/overview.md) — CharacterController が依存する RigidBody / Raycast
- [../Particle/overview.md](../Particle/overview.md) — WaterComponent が使用する ParticleEmitter
- [../Rendering/overview.md](../Rendering/overview.md) — Terrain / Water の描画パスとライティング設定
