# VFX — プレハブとしてのエフェクト (`.vfx`)

エフェクトを**シーンに置く普通の GameObject 階層**として作り、`.vfx` (= プレハブ) として保存・生成する。
専用のグラフ言語・専用のエディタ・専用の設定構造体をすべて捨て、
「シーンに置けるものはエフェクトにも置ける / エフェクトに置けるものはシーンにも置ける」を成立させる。

---

## 1. 現状と課題

### 1.1 `.vfx` のノード設定はコンポーネントの影のコピーになっている

[VFXGraphSystem.cpp:169-407](../../Projects/Engine/src/Scene/Systems/VFXGraphSystem.cpp) がやっているのは、
DAG ノードの設定を読んで **GameObject を生やし、既存コンポーネントへ写す**ことだけ。

| `.vfx` のノード設定 | 生成される実体 |
|---|---|
| `VFXTrailSettings` | `TrailComponent` / `MeshTrailComponent` |
| `VFXLightSettings` | `LightComponent` |
| `VFXDecalSettings` | `DecalComponent` |
| `VFXMeshSettings` | `MeshRenderer` + `MaterialComponent` |
| `VFXAnimatedMeshSettings` | `SkinnedMeshRenderer` + `MaterialComponent` + `AnimatorComponent` |
| `VFXForceFieldSettings` | `ParticleForceField` |
| `VFXScreenEffectSettings` / `VFXCameraShakeSettings` / `VFXTimeScaleSettings` | `VFXScreenEffect` / `VFXCameraShake` / `VFXTimeScale` |
| `VFXWindSettings` | `WindZoneComponent` |
| `VFXSubGraphSettings` | `VFXGraphComponent` (入れ子) |
| `particle` | `ParticleEmitter` — **これだけ既に実型 (`ParticleEmitterSettings`) を共用している** |

`particle` が実型を共用していることが答えになっている。`ParticleEmitter` に項目を足すと `.vfx` にも自動で乗るが、
`LightComponent` に項目を足しても `VFXLightSettings` には乗らない。乗せるには 4 箇所を追随させる必要がある。

1. `VFXxxxSettings` 構造体 ([VFXGraphAsset.hpp](../../Projects/Engine/include/Engine/Asset/VFXGraphAsset.hpp))
2. TOML codec ([VFXGraphAsset.cpp](../../Projects/Engine/src/Asset/VFXGraphAsset.cpp) — 1,492 行)
3. AI 向けスキーマ ([VFXAuthoringSchema.hpp](../../Projects/Engine/include/Engine/Asset/VFXAuthoringSchema.hpp))
4. ノード Inspector ([VFXGraphInspector.cpp](../../Projects/Editor/src/VFXEditor/Views/VFXGraphInspector.cpp) — 1,058 行)

**この 4 重メンテこそが「VFX に機能を足すのが重い」の正体**で、ノードを増やすほど増える。

### 1.2 エディタが 2 つある

[Projects/Editor/src/VFXEditor/](../../Projects/Editor/src/VFXEditor/) は **13,700 行の別アプリ**で、
Hierarchy・Inspector・ギズモ・選択・Undo・プレビューカメラをすべて VFX 専用に作り直している。
一方、本体エディタ側にはそれらが完成した形で存在する。

### 1.3 プレハブ基盤は完成しているのに 1 つも使われていない

`PrefabSerializer` は Save / Instantiate / Apply / Revert / PropagateToInstances / Override を備え、
`PrefabPool` は生成コストと `GameObject*` 無効化まで面倒を見ている。
にもかかわらず**ディスク上に `.prefab` が 1 つも無い**。エフェクトはその最も自然な最初の使い手。

**理由が実装側にあった (Phase 1 で判明)**: `PrefabSerializer` は Editor プロジェクトにしか無く、
`Script::InstantiatePrefab` のコールバックを注入しているのも
[EditorApp.cpp:959](../../Projects/Editor/src/EditorApp.cpp) だけだった。
`StandaloneProjectModule` は何も注入しないので、**配布ビルドではコールバックが未設定のまま
`InstantiatePrefab` が常に false を返し、`PrefabPool::Spawn` も必ず失敗していた**。
つまりプレハブは「エディタの中でしか存在できない機能」で、ゲームから使う道が無かった。
これは `.vfx` = プレハブの前提そのものを壊すので、Phase 1 で解消する (§11.1)。

### 1.4 パラメーター機構が文字列バインドになっている

`VFXParamBinding` は `schemaPath = "particle.colorStart"` のような文字列でノードのフィールドを指す。
型は実行時にしか合わず、綴りを間違えても保存も検証も通る。
`variants` / `subGraphForwards` / `signalNodes` / `signalOutputs` はいずれもこの上に積まれている。

### 1.5 AI の面が二重になっている

`vfx.*` の MCP ツールが 25 個ある (`vfx.node` / `vfx.link` / `vfx.nodeField` / `vfx.group` / `vfx.param` …)。
これは `node.*` / `component.*` / `transform.*` / `material.*` / `prefab.*` の VFX 版でしかない。

---

## 2. ゴール / 非ゴール

### ゴール

- **エフェクト = GameObject 階層**。シーンに置けるコンポーネントはすべてエフェクトの部品になる。
- **オーサリング = 通常のシーン編集**。Hierarchy / Inspector / ギズモ / ビューポート / Undo を流用する。
- **`.vfx` = プレハブ**。Instantiate / Apply / Revert / Override / Pool がそのまま効く。
- コンポーネントに項目を足したら**追随作業ゼロ**で VFX に乗る。
- 時間の見通し (どの層がいつ立ち上がっていつ消えるか) は失わない。

### 非ゴール

- ノードグラフによるオーサリングの温存。**旧 `.vfx` DAG と VFXEditor は削除する**。
- GPU パーティクルの仕組みそのものの変更。`ParticleEmitter` / `ParticleSimulationSystem` / `ParticlePass` は触らない。
- `.sequence` との統合。演出タイムラインは別レイヤーで、VFX を**呼ぶ**側に留まる。

---

## 3. アセット

`.vfx` の中身を `.prefab` と**同一の TOML スキーマ**にする。`PrefabSerializer` を拡張子引数付きにして共用する。

**WHY 拡張子を分けるか**: 中身が同じでも、AssetBrowser のフィルタ・色分け・サムネイル・作成メニュー・
ダブルクリック時の開き方 (プレビューシーン) ・`VFXRef` のドロップ受理は、すべて拡張子で分岐している。
「これは演出だ」という区別を失うと、Hierarchy に置く前提のプレハブと混ざって選べなくなる。

```
FX_IMP_Explosion.vfx
└─ FX_IMP_Explosion            [VFXComponent] [ImpactVfxComponent (script)]
   ├─ First Frame              (空 GameObject — 旧 group / pivot の置き換え)
   │  ├─ Flash Core            [ParticleEmitter]
   │  ├─ Splash                [ParticleEmitter]
   │  └─ Blast Core            [ParticleEmitter]
   ├─ Blast Volume             (空 GameObject)
   │  ├─ Blast Body            [ParticleEmitter]
   │  ├─ Shock Ring            [ParticleEmitter]
   │  └─ Arc Snap              [ParticleEmitter]
   ├─ Debris                   (空 GameObject)
   │  ├─ Sparks                [ParticleEmitter]
   │  ├─ Debris Smoke          [ParticleEmitter]
   │  └─ Heat Haze             [ParticleEmitter]
   ├─ Blast Light              [LightComponent] [VFXElement] [VFXLightEnvelope]
   ├─ Blast Push               [ParticleForceField] [VFXElement]
   └─ Ground Mark              [DecalComponent]
```

旧 `VFXGraphGroup` (Canvas の注釈枠) と `parentNodeId` の pivot GameObject は、
**どちらも「ただの空 GameObject」に一本化される**。見た目のまとまりと空間のまとまりが一致する。

---

## 4. 時間 — 「1 オブジェクトにつき時間の正本は 1 つ」

これが設計の中心。**二重管理を作らない**ために、時間を持つ場所を 1 つに決める。

### 4.1 自前で時間を持つコンポーネントには何も足さない

| コンポーネント | 既にある時間 |
|---|---|
| `ParticleEmitter` | `startDelay` / `duration` / `loop` / `bursts[].time` |
| `TrailComponent` | `duration` |
| `DecalComponent` | `lifetime` / `fadeTime` / `age` |
| `AudioSourceComponent` | クリップ長 |
| `AnimatorComponent` | ステートの尺 |

これらには `VFXElement` を**付けない**。付けると「どちらの duration が効くのか」が分からなくなる。

### 4.2 時間を持たないコンポーネントに `VFXElement` を付ける

`LightComponent` / `ParticleForceField` / `MeshRenderer` / `WindZoneComponent` /
`VFXScreenEffect` / `VFXCameraShake` / `VFXTimeScale` が対象。

```cpp
struct VFXElement {
    bool  enabled    = true;
    float startDelay = 0.0f;   // VFX 先頭からの遅延 [秒]
    float duration   = 1.0f;   // 0 以下 = ルートが終わるまで
    bool  loop       = false;
    // 生存窓 0..1 に対する重み。既定は矩形 (常に 1)。
    // Light の閃光・ScreenEffect のフラッシュ・CameraShake の減衰はこれ 1 本で足りる。
    ParticleCurve weightCurve;
    // 空でなければ時間ではなく VFXComponent::Trigger(name) で開始する。
    std::string trigger;

    // ランタイム出力
    float weight = 0.0f;       // 窓の外では 0
};
```

`VFXSystem` は毎フレーム、ルートの `time` から各 `VFXElement` の窓を判定して
**GameObject の active を切り替え**、`weight` を書く。旧 `VFXRuntimeNodeState::active` と 1:1 で対応する。

### 4.3 リンク (DAG) は startDelay へ畳む

旧 `VFXGraphLink` の `OnComplete` / `OnStart` + `delay` は、
`BuildVFXGraphSchedule` が既にトポロジカル評価して絶対時刻を出している。
**保存済みの絶対時刻を `startDelay` として持てば DAG は要らない。**

イベント系トリガー (`OnCollision` / `OnDeath` / `OnAnimationEvent` / `OnTrigger`) は
`VFXElement::trigger` の名前一致に置き換える。`VFXComponent::Trigger(name)` で該当要素を開始する。

**WHY DAG を捨てるか**: 実測で、既存 10 個の `.vfx` が持つリンク 86 本のうち **79 本が Entry からの
`OnStart` 直リンク**。残る 7 本も `Assets/VFX/Templates/Magic.vfx` の 1 ノードから伸びる `OnComplete` で、
これは「そのノードの duration」を足した絶対時刻に等しい。
`parentNodeId` を使っているアセットは **0 個**。
DAG が実際に表現していたのは「Entry からの遅延」だけで、グラフ canvas の複雑さに見合う情報を持っていない。

### 4.4 エンベロープ コンポーネント

`VFXElement::weight` を読んで実コンポーネントへ書く。VFX 専用ではなく**シーン上の普通のオブジェクトにも使える**。

| コンポーネント | 駆動先 | 旧設定の置き換え |
|---|---|---|
| `VFXLightEnvelope` | `LightComponent.intensity` / `.color` | `VFXLightSettings::intensityCurve` / `colorGradient` |
| `VFXTransformEnvelope` | `Transform.localScale` | `VFXMeshSettings::scaleStart/End/EasePower` |
| `VFXMaterialEnvelope` | `MaterialComponent.paramOverrides` | `VFXMeshSettings::colorStart/End` / `animatedParam` |
| `VFXDecalEnvelope` | `DecalComponent.opacity` / `.emissiveScale` | `VFXDecalSettings::fadeCurve` |

`VFXScreenEffect` / `VFXCameraShake` / `VFXTimeScale` は既に `weight` フィールドを持っているので、
`VFXElement` が直接書けばよい (専用エンベロープは不要)。これに伴い 3 つとも
`FBZZ_INTERNAL_COMPONENT` を外し、**手で追加できる通常コンポーネントへ格上げする**。

**これは Phase 2 でやる必要があった。** 3 つは内部型としてシリアライズ対象外で
(`SceneSerializer` に読み書きが 1 行も無い)、格上げしないと変換したプレハブから
画面演出が丸ごと消える。併せて `weight` は `VFXElement` が毎フレーム書くランタイム値なので
`Reflect` から外した (保存すると、読み込み直後の 1 フレームだけ前回の値で光る)。

これで旧実装が持っていた 3 種類バラバラのエンベロープ
(ScreenEffect の `fadeInTime`/`fadeOutTime`、CameraShake の `(1-t)^falloffPower`、
TimeScale の `blendInTime`/`blendOutTime`) が `weightCurve` 1 本に畳まれる。

**`Time::timeScale` の集計は 1 か所だけが持つ。** `VFXTimeScale` を集めて `Time::timeScale` へ書く処理は
「要求が無ければ 1.0 へ戻す」を含むため、2 つの system が同じことをすると片方の減速を
もう片方が同じフレームで踏み消す。移行期間中は `VFXSystem` が一手に引き受け
(読む対象の `VFXTimeScale` コンポーネントは新旧で共通なので集計は 1 回で足りる)、
`VFXSystem` は `After<VFXGraphSystem>` で旧経路の書き込み後に走る。

---

## 5. ルート — `VFXComponent`

```cpp
struct VFXComponent {
    bool  enabled     = true;
    bool  playOnAwake = true;
    bool  loop        = false;
    float speed       = 1.0f;
    // 0 以下 = 子から自動算出 (全要素の終了時刻の最大値)
    float duration    = 0.0f;
    // 再生終了で GameObject を破棄する。プール由来なら PrefabPool::Despawn へ返す。
    bool  autoDestroy = true;

    // ランタイム
    bool  playing = false;
    float time    = 0.0f;

    void Play(); void Restart(); void Pause(); void Resume(); void Stop();
    void Trigger(std::string_view name);
};
```

入れ子は「子に `VFXComponent` を持つ GameObject を置く」だけで成立する (旧 SubGraph の置き換え)。
再帰の深さガードは `VFXGraphComponent::nestingDepth` の実装をそのまま移す。

**エディタのスクラブ**は `time` を外から直接指定して、`Restart()` → 指定時刻まで固定ステップで進める。
`VFXPreviewController` の決定論スクラブの考え方をそのまま持ってくる。

---

## 6. パラメーター — 廃止してスクリプト公開フィールドへ

`VFXParamDefinition` / `VFXParamBinding` / `VFXVariantSet` / `VFXSubGraphForward` /
`VFXSignalNode` / `VFXSignalOutput` / `VFXParamOverride` をすべて削除する。

代わりに **`.vfx` のルートに置いた Script の公開フィールド**が唯一の入口になる。

```cpp
// GreenWare/Assets/Scripts/Vfx/ImpactVfxComponent.hpp
class ImpactVfxComponent : public Script {
    FBZZ_SCRIPT(ImpactVfxComponent)
public:
    FBZZ_FIELD(Vector4, polarityColor, "Polarity Color")
    FBZZ_FIELD_RANGE(float, sparkPower,  11.0f, "Spark Power",  2.0f, 26.0f)
    FBZZ_FIELD_RANGE(float, smokeAmount,  4.2f, "Smoke Amount", 0.0f,  8.0f)
    FBZZ_FIELD_RANGE(float, blastLight,  22.0f, "Blast Light",  0.0f, 60.0f)
    FBZZ_ASSET_FIELD(TextureRef, groundMark, "Ground Mark")

    void OnStart() override;   // 子を名前で引いて値を書く
};
```

呼び出し側 ([VfxManagerComponent.hpp](../../GreenWare/Assets/Scripts/Game/VfxManagerComponent.hpp)) は:

```cpp
GameObject* go = PrefabPool::Spawn(scene, impactVfxPath, point, rotation);
if (auto* p = go->GetComponent<ImpactVfxComponent>()) {
    p->polarityColor = ColorOf(polarity);
    p->sparkPower    = Lerp(sparkPowerMin, sparkPowerMax, strength01);
    p->blastLight    = ...;
    p->groundMark    = againstAnchor ? kMarkCrack : kMarkScorch;
}
go->GetComponent<VFXComponent>()->Restart();
```

**WHY**: `schemaPath` 文字列が消えて型が効く。ライトの強度を変えるつもりでパーティクルの色へ書く事故が
構造上起こらなくなり、Inspector の範囲・ツールチップ・グループもスクリプト側の宣言 1 箇所で決まる。

**注意**: スクリプトのフィールドは `Reflect` されていれば `.vfx` に保存される。
未 Reflect のフィールドは Play/Stop と DLL リロードで既定値へ戻る (既知の性質)。

---

## 7. スクリプト API

| 旧 | 新 |
|---|---|
| `VFXGraphComponent` (`graphPath` を持つ) | `VFXComponent` (プレハブ自身がグラフ) |
| `ScriptVFXProxy` の graph 操作 | `VFXComponent` の Play/Restart/Pause/Resume/Stop/Trigger へ付け替え |
| `std::vector<VFXParamOverride>` を積んで Emit | Spawn → スクリプトのフィールドへ代入 → Restart |
| — | `scene.SpawnVFX(VFXRef, position, rotation)` (`PrefabPool::Spawn` + autoDestroy のラッパ) |

`ScriptAssetType::VFX` / `VFXRef` はそのまま残る (拡張子を維持するため)。

---

## 8. エディタ

### 8.1 削除

- `Projects/Editor/{src,include}/**/VFXEditor/` 一式 (13,700 行)
- `VFXEditorLauncher` / `VFXPreview` パネル / `VFXPreviewCamera`
- `InspectorEffects` の VFX Graph 部分
- `ObjectPresets` の `fx.vfxGraph`
- AssetBrowser の「VFX Graph / Recipe から作成」メニュー

### 8.2 VFX プレビューシーン = 既存の Prefab 編集モード

**新しく作るものは無かった。** `EnterPrefabEditMode` / `SavePrefabEdit` / `ExitPrefabEditMode` /
`DrawPrefabEditBar` が既にあり、いずれも拡張子を見ずに `PrefabSerializer` を呼んでいる。
`.vfx` はプレハブなので、**振り分けを 1 箇所直すだけ**でそのまま乗る。

- `asset.open` の `.vfx` 分岐: 旧 DAG 形式なら旧 VFX Editor、プレハブ形式なら Prefab 編集モード
- `prefab.edit` が受け付ける拡張子に `.vfx` を追加 (旧形式は明示的に弾いて変換を促す)
- AssetBrowser のダブルクリックは既に `asset.open` へ委譲済みなので変更不要

得られるものは設計どおり: 現在のシーンを退避してその `.vfx` だけを開き、
**Hierarchy / Inspector / ギズモ / 選択 / Undo は本体のものをそのまま使う**。

`MaterialPreview` / `AnimationPreview` のような専用の隔離シーンを別に作らないのは、
それが «もう 1 つのエディタ» を生むから。今回捨てようとしているものと同じ構造になる。

### 8.3 VFX タイムライン パネル

開いている VFX ルート配下を **1 子オブジェクト = 1 トラック**で表示する。

- 横棒 = 生存窓。`ParticleEmitter` なら `startDelay` / `duration`、`VFXElement` ならその値を読む
  (§4.1 の「正本は 1 つ」がここでそのまま UI になる)。**書き戻し先も行ごとに覚える** —
  読んだ場所と違うところへ書くと、掴んで動かした値が別のフィールドへ入る
- 棒の端をドラッグして `startDelay` / `duration` を編集。Undo は**掴んで離すまでを 1 手**にする
  (ドラッグ中に積むと 1 回の調整で履歴が数十件になり、戻す操作が使い物にならない)
- 帯の色で「何で始まるか」を分ける: 時間 / ループ / 発火待ち (`trigger`)
- `duration <= 0` (= ルートが終わるまで) は縁を出して実尺と区別する
- 当たり判定は帯ではなくレーン全幅に置く。帯を幅 0 のボタンにすると 0.05 秒の窓が掴めない
- 空 GameObject は帯を持たない行として出る (層のまとめ役)
- スクラブは `VFXComponent::time` を直接指定する

**スクラブの正直な限界**: 曲線で駆動する層 (光・デカール・メッシュ・画面演出) は時刻だけで
決まるのでどこへ飛ばしても同じ絵が出る。一方パーティクルは «それまでの積み重ね» なので、
巻き戻すと頭から出直す。これはパネルに明示する — 黙っていると
「スクラブしたら煙が消えた」をバグとして追うことになる。

**スクラブの持ち主が消える問題**: 固定時刻を書くのはパネルだけなので、スクラブ中に
パネルを閉じるとその VFX は二度と動かなくなる。`editorScrubFrame` の鮮度を見て、
書き込みが 2 フレーム途絶えたら通常再生へ戻す (`ParticleEmitter::editorTimeScaleFrame` と同じ手)。

### 8.4 作成メニュー (`ObjectPresets`)

`Create > VFX >` に以下を足す。`ObjectPresetCatalog` に載せるだけで
**メニュー・コマンドパレット・MCP (`preset.catalog` / `preset.create`) の 3 面に同時に出る**。

| プリセット id | 生成物 |
|---|---|
| `vfx.root` | `VFXComponent` を持つ空ルート |
| `vfx.group` | 空 GameObject (層のまとまり) |
| `vfx.particle` | `ParticleEmitter` (loop なし・duration 1 秒) |
| `vfx.lightFlash` | `LightComponent` (影なし) + `VFXElement` + `VFXLightEnvelope` |
| `vfx.meshShell` | `MeshRenderer` + `MaterialComponent` + `VFXElement` + `VFXTransformEnvelope` + `VFXMaterialEnvelope` |
| `vfx.forceField` | `ParticleForceField` (Repulse) + `VFXElement` |
| `vfx.decal` | `DecalComponent` + `VFXDecalEnvelope` |
| `vfx.screenEffect` | `VFXScreenEffect` + `VFXElement` (立ち上がり付き weightCurve) |
| `vfx.cameraShake` | `VFXCameraShake` + `VFXElement` (減衰 weightCurve) |
| `vfx.timeScale` | `VFXTimeScale` + `VFXElement` |

**WHY 既存の `fx.*` と分けるか**: シーンに常設するエフェクト (焚き火・煙突) と、
1 発鳴らして消える演出では既定値が正反対になる。前者は `loop = true` で出しっぱなし、
後者は `loop = false`・`duration` 有限で終わったら自分で畳む。
同じプリセットに兼ねさせると、置いた直後の挙動がどちらでも間違う。

ルートを先に作る自動化はしない。`CreateObjectFromPreset` が親付けと選択を持っているので、
プリセット側が勝手に 2 つ作ると «どちらが選択されるか» が経路ごとに変わる。

### 8.5 Save as VFX

`PrefabSerializer` 側は変更不要だった (`WithPrefabExtension` は拡張子があればそのまま通す)。
Hierarchy の `SaveSelectedAsPrefab` を「サブフォルダと拡張子を受け取る」形に一般化し、
ルートに `VFXComponent` がある選択にだけ **Save As VFX** を出す (`Assets/VFX/*.vfx`)。

---

## 9. AI (MCP)

25 個の `vfx.*` を削除し、`node.* / component.* / transform.* / material.* / prefab.* / preset.*` に集約する。

| 残す | 理由 |
|---|---|
| `vfx.preview` / `vfx.previewMetrics` | 絵を見て評価する経路は代替不能。対象を `.vfx` プレハブへ付け替える |
| `vfx.lint` | オーバードロー・パーティクル数・光源数の警告。読む先を GameObject 階層へ付け替える |
| `vfx.optimize` | 同上 |
| `vfx.materialAnalyze` / `vfx.textureAnalyze` / `vfx.curvePresets` | 素材側の解析でグラフに依存しない |

`vfx.templateCatalog` は `preset.catalog` + `.vfx` プレハブのライブラリフォルダに置き換わる。

**これが今回の最大の副産物**: AI から見た VFX オーサリングが「専用 25 ツール」から
「シーン編集と同じ語彙」になり、AI が既に知っている操作でエフェクトを作れるようになる。

---

## 10. 移行 — 実際には「書き出し直し」で済んだ

当初は変換器 (`VFXLegacyConverter`) を作り、`VFXGraphSystem` の生成経路を使って
旧 DAG をプレハブへ展開する計画だった。実装まで済んだが、**採用しなかった**。

理由は 2 つ。

1. 変換器は旧経路 (`VFXGraphSystem` → `VFXGraphAsset`) に乗るため、**旧経路を消すまで消せない**。
   撤去の最後に「撤去できないもの」が 1 つ残る形になる。
2. `.vfx` の中身は結局 **TOML の書き換え**でしかなかった。particle 設定は
   `SerializeParticleEmitterSettings` が書いた表そのままで、プレハブ側も同じ codec を使う。
   つまり **`[nodes.particle]` を `[gameobjects.ParticleEmitter]` へ移すだけ**で見た目が保たれる。

実際にやったこと (旧アセットは git に残っているので、そこから起こした):

| 旧 | 新 |
|---|---|
| `[[nodes]]` (type 2) の `[nodes.particle]` | `[gameobjects.ParticleEmitter]` に**逐語コピー** |
| `[[links]]` の `delay` | `ParticleEmitter.startDelay` へ加算 (§4.1) |
| `[nodes.light]` | `LightComponent` + `VFXElement` + `VFXLightEnvelope` |
| `[nodes.decal]` | `DecalComponent` (`lifetime` = ノードの duration) + `VFXDecalEnvelope` |
| `[nodes.forceField]` | `ParticleForceField` + `VFXElement` |
| `[[groups]]` (Canvas の枠) | 空 GameObject の**層**。矩形に含まれるノードがその子になる |
| `[[parameters]]` / `[[bindings]]` | ルートに載せたスクリプト (§6)。既定値は旧 default と同じ |

**逐語コピーが成立するのが肝**。「ノード設定はコンポーネントの影のコピーだった」(§1.1) という
出発点の観察が、そのまま移行の手順になっている。写す先が同じ型なら、写すのに変換は要らない。

移行できたのは GreenWare の 5 本。`Assets/VFX/Templates/` の 5 本は
「プレハブ流のお手本」として作り直す前提で捨てた (§12)。


## 11. 移行の記録

### 11.1 前提だった: プレハブ生成を Engine へ移す

§1.3 のとおり、生成経路が Editor にしか無いままでは `.vfx` が配布ビルドで再生できない。
**「読んで展開する」側だけを Engine へ移し、オーサリング操作は Editor に残した。**

| 移した先 | 中身 |
|---|---|
| `Engine/Scene/PrefabInstantiate.{hpp,cpp}` (新設) | `InstantiatePrefabAsset` / `PrefabOverride` / `PrefabOverrideSet` / `FindNodeAtPath` / `SetNodeAtPath` |
| `Editor/Util/PrefabSerializer` (残る) | `SaveSelection` / `Apply` / `Revert` / `PropagateToInstances` — `Instantiate` は Engine へ委譲する 1 行 |
| `Editor/Util/PrefabOverrides` (残る) | `ComputePrefabOverrides` / `WithoutEntry` / `FormatNodeForDisplay` (差分の**算出**はオーサリング) |

併せて 3 つ手当てした。

- `SceneSerializer::LoadFromText` を追加。プレハブ展開の補完パスが、メモリ上の TOML を読むためだけに
  一時ファイルを書いていた。`sourcePath` は表示用ではなく、`TerrainComponent` の相対参照を解決する基準。
- `Script::InstantiatePrefab` はコールバック未設定なら `InstantiatePrefabAsset` を既定として使う。
- パス解決は `AssetDatabase::ProjectRoot()` を使って生成側が行う。

### 11.2 実際に通った順序

| Phase | 内容 |
|---|---|
| **1. 基盤** | §11.1 + `VFXComponent` / `VFXElement` / エンベロープ 4 種 / `VFXSystem` |
| **2. 格上げ** | `VFXScreenEffect` / `VFXCameraShake` / `VFXTimeScale` を通常コンポーネントへ (内部型のままでは保存されず、プレハブから画面演出が消える) |
| **3. エディタ** | Prefab 編集モードへの `.vfx` 振り分け / VFX Timeline / 作成プリセット 10 種 / Save As VFX |
| **4. GreenWare** | パラメーター script 化 5 本 + `VfxManagerComponent` 書き換え |
| **5. 撤去 + 再生成** | 旧経路をすべて削除し、GreenWare の 5 本を新形式で書き起こし |

### 11.3 撤去の実績

```
168 files changed, 1,514 insertions(+), 39,326 deletions(-)
```

| 分類 | 対象 |
|---|---|
| Engine | `VFXGraphAsset` / `VFXParameter` / `VFXParameterRuntime` / `VFXAuthoringSchema` / `VFXGraphComponent` / `VFXGraphSystem` |
| Editor | `VFXEditor/` 24 ファイル / `VFXEditorLauncher` / `VFXPreview` / `VFXPreviewCamera` / `VFXLegacyConverter` |
| プロセス | `Projects/VFXEditorLauncher/` (`FBZZVFXEditor.exe`) — CMake・SDK 検証・.vscode 構成からも除去 |
| MCP | `vfx_*` ツール 50 個と対応する契約・テスト |
| アセット | 旧 `.vfx` 35 ファイル |

**残したもの**: `vfx_generate_motion_vectors`。フリップブックアトラスから
モーションベクターを作るテクスチャ処理で、旧グラフに依存していない。

**付け替えたもの**: `ScriptVFXProxy` → `VFXComponent` (パラメーター操作は §6 のとおり削除)、
`SequenceSystem` の VFX トラック、`effects.control` operator、
`viewport.capture` の `view` (`vfx` を削除 — 専用プレビュー面が無くなったため)。

## 12. 未決

- **`.vfx` プレビューシーンの背景**: グリッドのみか、明暗を切り替えられるようにするか
  (加算ブレンドの見え方は背景の明るさで大きく変わる)
- **`VFXElement` の `trigger`** をイベント名の自由文字列にするか、列挙にするか
- **Templates 5 個の作り直し方針**: 「プレハブ流のお手本」として何を見せるか。
  旧 Templates は捨てたので `Assets/VFX/Templates/` は空。作成プリセット (§8.4) が
  «部品を置く» までは面倒を見るため、Template が引き受けるべきは
  «層構成と描画順» だけになった。1 本ずつのお手本より、
  `FX_IMP_Explosion` のような実物を読ませる方が早い可能性がある。
- **企画書 12.5 の「引き寄せの線」が未着手**: 集束時に全員から起爆点へ線を伸ばす表現
  (`×5 CHAIN` の規模が起爆前に読める) は、対象が 2 点間を結ぶ動的な線なので
  `.vfx` プレハブ 1 個では表せない。`LineRenderer` をスクリプトで張る側の仕事になる。
