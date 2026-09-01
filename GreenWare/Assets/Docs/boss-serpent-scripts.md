# Boss 2：ポラリティ・サーペント — 実装するスクリプト

> 親: [boss-serpent.md](boss-serpent.md)（設計と寸法の出どころ）／ [boss.md](boss.md)（ボス1）
> 置き場: `Assets/Scripts/Combat/`（`Serpent*`）
> シーン: `Assets/Scenes/Stage_02.scene`（`Boss02` と `Boss02_Arena_Map` は配置済み）

この文書は**何を書くか**だけを決める。寸法や挙動の理由は `boss-serpent.md` にあり、
ここでは「そこで決まった値をどのクラスが持つか」を割り当てる。

---

## 全体像

```
                    SerpentBossComponent          IBoss + IDamageable。HP と段階
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
  SerpentAiComponent    SerpentBodyComponent   SerpentPolarityRigComponent
   手を選ぶ / 実行        長さ・折る・露出         節の極と輪郭
        │                     │                     │
        ├──────────┬──────────┘                     │
        │          │                                │
 SerpentPath  SerpentSpine ◄──────────────── SerpentHitboxRigComponent
  経路を作る    30 骨を沿わせる                 節ごとの当たり
        │
        ▼
 SerpentApertureComponent   ← マップ側 (Boss02_Arena_Map に付ける)
  16 口の開閉と予兆
```

**依存の向きは一方通行**。下ほど「言われたとおり動かすだけ」で、上ほど「決める」。
ボス1で `BossPolarityRigComponent` → `BossCollapsePostureComponent` の参照が
循環しかけたので、**下から上を引かない**こと（押し込む側が上）。

---

## 実装順

`boss-serpent.md` の実装順に対応する。**1 と 2 が動くまで他は試せない。**

| | スクリプト | これが出来ると |
|---|---|---|
| 1 | `SerpentApertureComponent` | 口が開く。予兆が光る |
| 2 | `SerpentSpineComponent` + `SerpentPathComponent` | 穴から出て別の穴へ潜る |
| 3 | `SerpentHitboxRigComponent` | 胴に当たれる／斬れる |
| 4 | `SerpentPolarityRigComponent` | 節に極が乗る。輪郭で読める |
| 5 | `SerpentBodyComponent` | 胴が折れて短くなる |
| 6 | `SerpentAiComponent` | 手が出る |
| 7 | `SerpentBossComponent` | HUD・進行に繋がる |

---

## シーン側の契約（すでに出来ているもの）

スクリプトはこの名前で引く。**名前が命綱なので、リネームしたらここも直す。**

| ノード | 数 | 備考 |
|---|---|---|
| `ARENA_Rim_<口>` | 16 | 予兆で光る縁。`M_AR_RimGlow` |
| `ARENA_Collar_<口>` | 16 | 回る輪 |
| `ARENA_ShutterA_<口>` / `ARENA_ShutterB_<口>` | 32 | 羽 2 枚 |
| `COL_Shutter_<口>` | 16 | 閉じている間だけ有効な床 |
| `Seg28` … `Seg01` / `Head` | 29 | 変形骨（親は `Root`、尾が根） |
| `E_<材質>_<部位>` | 116 | 部位は `Head` と `S01`…`S28` |
| `SPAWN_Player` / `SPAWN_Serpent` | 2 | 位置の目印（EMPTY） |

口の id は内輪 `I1`…`I6`（r = 8.0 m）、外輪 `O1`…`O10`（r = 16.0 m）。開口半径 2.2 m。

> **口の位置は `ARENA_Rim_<口>` から引けない（実装時に判明）。**
> 取り込み時にノードの変換は頂点へ焼かれていて、16 口の Transform は全部
> `position = (0,0,0)`。`worldPosition` を読むと 16 口がアリーナの中心に重なる。
> 位置を持っているのはメッシュだけで、そこはスクリプトから読めない。
>
> 代わりに `SerpentApertureComponent` が **輪の半径・口数・位相を公開フィールドで持ち、
> そこから解く**。既定値は書き出し済みメッシュのサブメッシュ境界球を実測して合わせてある
> （`I1 = (8, ·, 0)` / `O1 = (15.2169, ·, 4.9443)`）。軸の対応は
> **Blender (x, y) → エンジン (x, z)**（cos が x / sin が z）。
> `serpent_map_build.py` の `RINGS` を直したらここも直すこと。

---

## 1. SerpentApertureComponent

`Assets/Scripts/Combat/SerpentApertureComponent.hpp`
**付ける先: `Boss02_Arena_Map`**（ボスではない。口は場の設備）

16 口の開閉と予兆を持つ。**リグもクリップも使わない** — 剛体の平行移動と、
口の中心まわりの回転だけで足りる（理由は `boss-serpent.md`「開口の動き」）。

### 公開フィールド

```cpp
FBZZ_FIELD_RANGE(float, telegraphSeconds, 0.60f, "Telegraph", 0.0f, 3.0f)  // 18F
FBZZ_FIELD_RANGE(float, unlockSeconds,    0.20f, "Unlock",    0.0f, 2.0f)  //  6F
FBZZ_FIELD_RANGE(float, sinkSeconds,      0.13f, "Sink",      0.0f, 2.0f)  //  4F
FBZZ_FIELD_RANGE(float, slideSeconds,     0.27f, "Slide",     0.0f, 2.0f)  //  8F
FBZZ_FIELD_RANGE(float, lockDegrees,  22.5f, "Collar Turn", 0.0f, 45.0f)
FBZZ_FIELD_RANGE(float, sinkMeters,    0.30f, "Sink",   0.0f, 1.0f)
FBZZ_FIELD_RANGE(float, slideMeters,   2.53f, "Slide",  0.0f, 5.0f)
FBZZ_FIELD_RANGE(float, glowStrength,  6.0f,  "Glow",   0.0f, 20.0f)
FBZZ_FIELD_READ_ONLY(std::string, debugOpen, "-", "Open")   // 開いている口の一覧
```

### 公開 API

```cpp
void Open(const std::string& hole);      // 予兆から開くまで
void Close(const std::string& hole);     // 逆順
bool IsOpen(const std::string& hole) const;
Vector3 HoleCenter(const std::string& hole) const;   // ARENA_Rim_<口> の位置
const std::vector<std::string>& Holes() const;       // "I1".."O10"
```

### 中身

- `OnStart()` で 16 口ぶんの `EntityRef` を名前で引いて控える。
  **DLL リロードで `Script` は作り直され `EntityRef` は空へ戻る**ので、
  `BossCollapsePostureComponent::EnsureTargets()` と同じく「空なら引き直す」で書く。
- 動きは `ScriptTweenProxy` + `StartCoroutine`。
  ヘッダの設計意図がまさに «扉が 0.4 秒かけて開く» なので、`OnUpdate` に経過時間を
  足す書き方はしない。
  ```cpp
  Coroutine OpenHole(std::string hole) {
      SetGlow(hole, glowStrength);
      co_await WaitForSeconds(telegraphSeconds);
      StartCoroutine(CollarOf(hole)->tween.RotateTo(q, unlockSeconds, TweenEase::OutCubic));
      co_await WaitForSeconds(unlockSeconds);
      // 沈む → 滑る
  }
  ```
- **カラーの回転は口の中心まわり。** 取り込まれたノードの原点は口の中心ではなく
  ワールド原点なので、`T(P)·R·T(-P)` を「回転 + 位置」の 2 つへ畳んで入れる
  （`BossCollapsePostureComponent::OnUpdate` に同じ形の実装がある）。
  ```cpp
  const Quaternion rot   = Quaternion::FromAxisAngle(Vector3::UP, rad);
  const Vector3    moved = rot * center;
  collar->transform.rotation = rot;
  collar->transform.position = center - moved;
  ```
- **羽は下げてから外へ。** `(0, -0.30, 0)` の後に `法線 × ±2.53`。
  真横へ滑らせると床スラブと交差する。法線は床面（エンジンでは XZ 平面）で
  口の中心から見た接線に直交する向き。
- `COL_Shutter_<口>` は開き始めに `SetActive(false)`、閉じ終わりに `true`。
- **材質は口ごとにインスタンスを持つこと。** 16 個が `M_AR_RimGlow` を共有したまま
  パラメータを触ると、アリーナ中の縁が一緒に光って予兆が予兆でなくなる。
  `MaterialComponent` は GameObject ごとなので、`material.SetColor` 等は
  対象の GameObject を指定して呼ぶ。

---

## 2. SerpentPathComponent / SerpentSpineComponent

### SerpentPathComponent
`Assets/Scripts/Combat/SerpentPathComponent.hpp` — **付ける先: `Boss02`**

「どの穴から出てどの穴へ入るか」を受け取り、**円弧の経路**を作る。

```cpp
void SetRoute(const std::string& from, const std::string& to, float exposedMeters);
Vector3 At(float s) const;      // 弧長 s の位置。範囲外は端の接線で延ばす
float   Length() const;
```

- **経路は必ず円弧。** 同じ弦と弧長を満たす形のうち最大曲率が最小になるのが
  曲率一定の円弧で、正弦の山だと曲がりが頂上に集中して 2 倍近く折れる
  （実測 30.2度 対 58.9度）。上限は 1 関節あたりなので、どこか 1 箇所でも
  超えれば破綻する。
- 床下は端の接線でまっすぐ延ばす。
- 弦（穴の間隔）が **8.0 〜 11.5 m** に収まっているかを検算し、外れていたら
  `debug.LogWarning` で名指しする。狭いと折れすぎ、広いと胴が床に埋まる。
- 参照実装は Blender 側の `serpent_arena.py` の `arch()` / `route()` / `at()`。
  **同じ式なので、片方を直したらもう片方も直す。**

### SerpentSpineComponent
`Assets/Scripts/Combat/SerpentSpineComponent.hpp` — **付ける先: `Boss02`**

30 本の骨を経路へ沿わせる。**追従（follow-the-leader）**で、
頭の弧長位置から後ろへ骨の長さぶんずつ遡って関節を取る。

```cpp
FBZZ_FIELD_RANGE(float, headSpeed, 6.0f, "Head Speed", 0.0f, 30.0f)
FBZZ_FIELD_READ_ONLY(int,   debugExposed, 0,    "Exposed")   // 地上に出ている節
FBZZ_FIELD_READ_ONLY(float, debugBend,    0.0f, "Max Bend")  // 胴の最大折れ角
```

- 骨の並びは尾 → 頭：`Seg28` … `Seg01` → `Head`。親は `Root`。
- 骨の長さは `OnStart` で 1 度だけ実測して控える（節 0.80 m、頭 1.90 m）。
  定数で持つと、モデルを割り直したときに黙ってずれる。
- 関節から**ローカル回転**へ落とす。`transform.worldRotation` は
  TransformSystem が毎フレーム上書きするので直接書かない
  （`Transform.hpp` に「world 値は直接変更しない」とある）。
  親から順に、`local = parentWorld.Inverse() * desiredWorld` で入れる。
- **折れ角の判定は胴と頭を分ける。** 頭は 1.9 m で節の 2.4 倍長いため、同じ曲率でも
  頭の関節だけ余計に回る（胴 11.8度 のとき頭 16.5度）。実測では首は 40度 まで
  噛まないので、まとめて最大を採ると誤診する。胴の上限は **12度/関節**。
- `debugExposed` は「地上に出ている関節の数」。設計上は常に **14 節**。

> **アニメーションクリップは焼かない。** 16 口のどこへでも潜る動きはクリップでは
> 表せない。Blender 側のスプライン IK はプレビュー専用。

---

## 3. SerpentHitboxRigComponent

`Assets/Scripts/Combat/SerpentHitboxRigComponent.hpp` — **付ける先: `Boss02`**

ボス1の `BossHitboxRigComponent` と同じ作り。骨に沿って当たりを生成し、
**節を名指しで引ける**ようにする。

```cpp
GameObject* SegmentBone(int index) const;      // 1..28 と Head
int SegmentOf(GameObject* hit) const;          // 当たった当たりから節番号
bool IsExposed(int index) const;               // 地上に出ているか
```

- `OnStart` で `Seg01`…`Seg28` と `Head` を部分木から引き、カプセルを並べる。
  骨名はシーン内で一意でないので**必ずボスの部分木だけを見る**
  （ボス1が `FindInSubtree` を使っているのと同じ理由）。
- 引けなかった骨の数を数えて、0 でなければ `debug.LogError` で名指しする。
  綴り違いは「その部位だけ当たらない」という形でしか出ない。
- **頭だけ斬撃が通る。** 胴は極を乗せる対象で、斬っても削れない（ボス1と同じ）。

---

## 4. SerpentPolarityRigComponent

`Assets/Scripts/Combat/SerpentPolarityRigComponent.hpp` — **付ける先: `Boss02`**

節ごとの極と、その表示。ボス1の `BossPolarityRigComponent` が
`E_*_<接尾辞>` を集めて `std::vector<EntityRef> meshes` に積んでいるのと同じ形。

```cpp
void     SetPolarity(int segment, Polarity p);
Polarity PolarityOf(int segment) const;
bool     CanTakePolarity(int segment) const;   // 地上に出ている節だけ
```

- 部位名は `Head` と `S01`…`S28`。`E_<材質>_<部位>` の 4 メッシュで 1 節。
- **極は輪郭（ポストプロセス）で出す。**発光帯（`M_E_Ring_Idle`）は
  «電気が通っている» だけを示す中立の琥珀で、極性の表示ではない。
  赤で灯しっぱなしにすると極を乗せていない節まで ＋ に見える。
- **節ごとに絶縁されている。** 雑魚の Serpent は導体で 1 発で全節が帯電するが、
  こちらは伝わらない。1 節ずつ塗る。
- **ボスは極を塗り替えない。** 妨害は潜行して仕込みを射程外へ逃がすことだけ。

---

## 5. SerpentBodyComponent

`Assets/Scripts/Combat/SerpentBodyComponent.hpp` — **付ける先: `Boss02`**

胴の長さと、折る操作を持つ。

```cpp
int  SegmentCount() const;                 // 28 から減る
bool TryFold(int a, int b);                // 逆極の 2 節。間を潰して繋ぐ
int  ExposedCount() const;                 // 常に 14 (残りが 14 を切ったら全部)
```

- **離れた 2 節を逆極にすると輪が閉じ、間の節が潰れて排出され、両端が溶接されて
  胴は繋がったまま短くなる。**千切って後ろを落とすのではない
  （3 節目と 7 節目を組んだだけで 8〜16 節が丸ごと消えてしまう）。
- 削れる長さ = **その 2 節の間にある節の数**。近い組は 1〜2 節、遠い組は 4〜5 節。
- 潰した節の破片は**床下へ落とす**。その場に残さない
  （盤面に帯電体が増えると «離れた 2 節を選ぶ» が成立しなくなる）。
- 段階はフェーズを別に持たず**長さがそのまま段階**：
  28〜23 節 / 22〜15 節 / 14 節以下。

---

## 6. SerpentAiComponent

`Assets/Scripts/Combat/SerpentAiComponent.hpp` — **付ける先: `Boss02`**

手を選んで実行する。ボス1の `BossAiComponent` と同じ位置づけ。

| 手 | 予兆 | 動き | 尺 |
|---|---|---|---|
| 突き上げ | 穴の縁が光る | 足元の穴から節が跳ね上がる | 18F |
| 薙ぎ | 胴が一度しなる | 露出した胴が水平に薙ぐ | 27F → 15F |
| 頭の突進 | 喉のコアが溜まる | 8 m 直線、外すと硬直 36F | 21F → 10F |
| 囲い込み | 複数の穴が同時に光る | 周囲に胴の檻を作る | 36F |
| 締め上げ | 檻の節が明滅 | 檻が縮む | 120F |
| 潜行 | 節が沈み始める | 露出節を引っ込め別の穴から出る | 30F |

- 手の選択は**残り節数**で切り替える（`SerpentBodyComponent::SegmentCount()`）。
- 渡る 2 口は「間隔が 8.0〜11.5 m」の組から選ぶ。外れた組を選ぶと
  `SerpentPathComponent` が警告を出す。
- 30fps 前提。フレーム数は秒へ直して `FBZZ_FIELD` で出す
  （調整は Inspector でやりたいので定数に埋めない）。

---

## 7. SerpentBossComponent

`Assets/Scripts/Combat/SerpentBossComponent.hpp` — **付ける先: `Boss02`**

```cpp
class SerpentBossComponent : public Script, public IDamageable, public IBoss {
    FBZZ_SCRIPT_DERIVED(SerpentBossComponent, Script, IDamageable, IBoss)
```

`IBoss` が要求するもの（`IBoss.hpp`）:
`BossName()` / `CurrentPolarity()` / `PolaritySwitchRemaining()` /
`CurrentPhase()` / `PhaseCount()` / `IsStaggered()` / `IsEngaged()`

`IDamageable` が要求するもの:
`ApplyDamage(int)` / `CurrentHealth()` / `MaxHealth()` / `IsAlive()`

- **HP は `IDamageable` の口で受ける。**`IBoss` に重ねない（ボスバーがどちらを
  読むかで割れる）。
- `CurrentPhase()` は残り節数から出す。フェーズ変数を別に持たない。
- `CurrentPolarity()` は**この蛇では意味を持たない**（ボスは極を塗り替えない）。
  `Polarity::Neutral` を返し、その理由をコメントに書く。

---

## 落とし穴（この作業で実際に踏んだもの）

- **`FBZZ_SCRIPT(...)` マクロを忘れると 67 個のエラーが出る**
  （`'_fbzz_base': 定義されていない識別子です`）。新しいクラスを足したら最初に書く。
- **`scene.GetScript<T>()` は引数なしだと自分の GameObject。**他所のを引くときは
  `scene.GetScript<T>(go)`。
- **`EntityRef` は DLL リロードで空へ戻る。**「作った」を覚えたままだと
  生成物が 1 組ずつ増え続ける。「空なら引き直す」で書く。
- **`transform.worldPosition` / `worldRotation` は書かない。**TransformSystem が
  毎フレーム上書きする。ローカルへ入れること。
- **`.meta` は手で書かない。**ただし `.fbx.meta` の取り込み設定
  （`unit_scale_multiplier` / `selected_meshes`）だけは例外的に手で直している。
- **ノード名の連番に注意。**Blender 側でメッシュデータが残ると `ARENA_Steel.003` の
  ように書き出され、名前で引く実装が全部外れる。ビルダー側に検算を入れてあるが、
  シーンのノード名も `.` が付いていないか確認すること。

---

## 設計からの変更点（実装して判った分）

| 企画 | 実装 | なぜ |
|---|---|---|
| `SerpentBossComponent : Script, IDamageable, IBoss` | `IBoss` だけ。HP は `EnemyHealthComponent` | `IDamageable::Registry()` は GameObject をキーにした 1 対 1 の名簿で、2 つ載せると後から名乗った方が黙って上書きする。しかも `GameFlowComponent` / `BossHealthBarComponent` は既に `EnemyHealthComponent` を名指しで引いていて、無いとステージが終わらない。ボス1の `BossPolarityCoreComponent` と同じ形に揃えた |
| `SerpentBodyComponent::ExposedCount()` | `SerpentSpineComponent` が持つ | 露出は経路と頭の位置で決まる。Body に置くと Body → Spine → Body で include が輪になる |
| 口の位置は `ARENA_Rim` の `worldPosition` | 輪の式から解く | 上の「シーン側の契約」参照 |
| — | `SerpentBones.hpp` を追加 | 骨名・部位名・当たり名を 3 箇所で綴らないため（`BossTelegraph.hpp` と同じ「アタッチしないユーティリティ」） |

### エンジン側に足したもの

**`AnimatorComponent::externalPose`**（`Boss02` で `true`）。

スキニングパレットを埋めるのは `AnimatorSystem` だけで、`SkinningComputePass` も
各 GeometryPass も**親をたどって Animator を探す**。つまり:

- Animator を付けないと → 骨を動かしてもメッシュは bind pose のまま描かれる
- Animator を付けると → クリップが無いフレームは `ApplyBindPoseToBones` が
  Script の書いた姿勢を毎フレーム消す

`externalPose` はその 3 つ目の道で、**戻さずに今の骨からパレットを組み直す**
（`PropagateBoneTransforms` → `RebuildSkinningFromBoneTransforms`）。
Inspector の Animator に「External Pose」として出る。

> **ビルド順**: `AnimatorComponent` のレイアウトが変わるので、
> **FBZZSDK → Scripts.dll** の順で建て直すこと（古い SDK ヘッダーのままだと ABI がずれる）。

### 部位が明滅する（カリングの球がバインドポーズに取り残される）

**症状:** `Boss02` の部位が、カメラを振ると 1 枚ずつ独立して現れたり消えたりする。

**原因はオクルージョンではなく視錐台カリング。** スキンドメッシュはそもそも
オクルージョンの対象になっていない（`ForwardPasses` / `DeferredPasses` の
`TestAndRaster` は `MeshRenderer` の静的メッシュだけを通す）。
効いていたのは `IsSkinnedVisible` → `ComputeSkinnedWorldBounds` の方で、
ここは**バインドポーズの submesh バウンズを Renderer の Transform で運んでいるだけ**だった。

蛇の場合、書き出し済みの bind bounds はこう並んでいる（`Serpent.fzasset`）:

| submesh | bind bounds の中心 | 半径 |
|---|---|---|
| `E_ArmorGrey_Head` | (0.00, 0.99, **+11.20**) | 1.45 |
| `E_ArmorGrey_S07` | (0.00, 1.41, **+5.01**) | 1.60 |
| `E_ArmorGrey_S28` | (0.00, …, **−11 付近**) | … |

つまり 116 個の球が**原点を通る 22 m の直線上に釘付け**になっている。一方、実体の胴は
半径 22 m のアリーナのどこへでも行く（潜行中は −40 m）。球と実体が最大 30 m 離れるので、
カメラを振ると「その球が錐台から外れた部位だけ」が消える ── 部位ごとに独立して明滅する。

`Boss02` の Transform は原点から動かない設計（カメラの注視先・進行・HUD がそこを見る）
なので、Transform を追従させて直すことはできない。

**直した内容（エンジン）:**

- `AnimatorComponent` に `skinnedBoundsCenter` / `skinnedBoundsRadius`（owner ローカル・
  ランタイム専用・シリアライズしない）を追加
- `AnimatorSystem::UpdateSkinnedBounds()` が、ポーズを組み終えたフレームだけ
  **骨の実際の位置**から球を組み直す（`nodeGlobalTransforms` に
  `rootInverseTransform` を掛けて owner ローカルへ戻す）。
  停止中・スケルトン無し・評価に失敗したフレームは半径 0 のままで、
  カリングは従来のバインドポーズ球へそのまま落ちる
- `ComputeSkinnedWorldBounds()` は第 1 引数を `Transform` から `GameObject` へ変え、
  親をたどって Animator を探す。骨から作った球があればそれを使い、
  **その Renderer が描く submesh のバインド半径**を「肉の厚み」として足す
  （Animator が持っているのは骨の広がりだけ。厚みは Renderer ごとに違うので、
  Animator 側で一律に足すと必ず過大になる）

> **代償:** 1 つのモデルの全 submesh が同じ球を共有するので、部位ごとの細かい
> カリングは効かなくなる（蛇なら 116 枚が「全部描く / 全部落とす」になる）。
> `ComputeSkinnedWorldBounds` の元のコメントが避けようとしていたのはこれだが、
> **間違った場所の細かい球より、正しい場所の粗い球を採る**。
> 部位ごとに戻すなら、submesh がどの骨に紐付いているか（`cpuSkinnedVertices` の
> `boneIndices`）をメッシュ側へ 1 度だけ焼き込む必要がある。

> **ビルド順**: `AnimatorComponent` のレイアウトがまた変わる。
> **FBZZSDK → Scripts.dll** の順で建て直すこと。

### 経路グラフ（実測）

窓を **8.0 – 10.5 m** にしたときの、口ごとの渡り先。**グラフは連結（16/16）で行き止まりは無い。**

| 口 | 渡り先 |
|---|---|
| `I1` | `O1`(8.75) `O10`(8.75) |
| `I2` | `O2`(8.09) `O3`(9.91) |
| `I3` | `I4`(8.00) `O4`(8.09) `O3`(9.91) |
| `I4` | `I3`(8.00) `O5`(8.75) `O6`(8.75) |
| `I5` | `I6`(8.00) `O7`(8.09) `O8`(9.91) |
| `I6` | `I5`(8.00) `O9`(8.09) `O8`(9.91) |
| `O1`…`O10` | それぞれ 3〜4 本（隣の外輪 9.89、内輪の 8.09 / 8.75 / 9.91） |

弦の種類は 5 つだけで、それぞれ弧の形が決まる。

| 弦 | 組数 | 山 | 胴の折れ | 射程内の関節 | 床下の折れ | 最深 |
|---|---|---|---|---|---|---|
| 8.00 m | 2 | 3.28 m | 11.23度 | 11 / 14 | 5.94度 | −6.20 m |
| 8.09 m | 4 | 3.24 m | 11.06度 | 11 / 14 | 5.78度 | −6.20 m |
| 8.75 m | 4 | 2.95 m | 9.71度 | 14 / 14 | 4.62度 | −6.18 m |
| 9.89 m | 10 | 2.25 m | 6.98度 | 14 / 14 | 2.53度 | −5.70 m |
| 9.91 m | 4 | 2.23 m | 6.93度 | 14 / 14 | 2.49度 | −5.69 m |

「射程内の関節」は剣の射程 2.6 m ＋ 節の太さで、山の天辺に届くかどうか。
**内輪どうし（8.00 m）だけは天辺の 3 関節が届かない** ─ 脇を狙う経路になる。

> **企画の上限 11.5 m は成立しない。** 弧長は露出長（11.2 m）で固定なので、
> 弦がそれに近づくほど弧は伸びきる。11.39 m の 4 組（`I2`–`O1` / `I3`–`O5` /
> `I5`–`O6` / `I6`–`O10`）は**弦の方が弧より長く**、丸めの結果
> 「山 0.00 m・半径 56000 m」── 床に寝た棒になっていた。
> 10.5 m で切っても各口に 2 本以上残り、グラフは連結のまま。
>
> **床下も接線のままでは破綻していた。** 弦 8 m の口での接線は水平から 78 度あり、
> そのまま 11.2 m 延ばすと y = −11.0 m ── 床下スラブ（`ARENA_Pit`, −7.4 m）の下まで
> 突き抜けて、開いた口から「床を貫いた胴」が見えていた。深さ 6.2 m で水平になる
> 円弧へ繋ぐよう直した（`SerpentPathComponent::Underground`）。

### 動きの作り込み

| | どこ | 何を |
|---|---|---|
| 蠕動 | `SerpentSpineComponent` | 経路に直交する水平へ進行波。**折れ角の余地に比例**して自動で縮むので、狭い経路では勝手に大人しくなる。両端（口の真上）では 0 へ落として、胴が縁を舐めないようにする |
| 頭の狙い | 同上 `SetAim()` | 首 4 関節へ**角を分けて**積む。1 本で 45 度回すと首の 1 関節が限界を超えるが、割れば上限の内側に収まる。頭が床下にいる間は掛からない |
| 狙う / 狙わない | `SerpentAiComponent` | 構え 1.0 ／ 突き上げ・突進の溜め 1.0 ／ 渡り中 0.45 ／ 突進中 0.3 ／ 潜行と登場は 0。**潜っている最中に振り返ると「逃げている」が読めない** |
| 経路選び | 同上 | 並べ替えの基準を「入る口の距離」から**「弧の真ん中がプレイヤーにどれだけ近いか」**へ。相手にするのは口ではなく胴で、胴が来るのは 2 口の間 |
| 往復の禁止 | 同上 | 直前に出てきた口へは戻らない（他に選べるときだけ）。2 口の往復に落ちると 16 口が「1 本の橋」に見える |

> **蠕動が要る理由:** 構えている間 `DriveHeadArc` は同じ弧長を指し続けるので、
> 直前の実装では 24 m の胴が 1 フレームも動かなかった。攻撃と攻撃の間が
> 「置物になる数秒」で埋まっていて、そこが一番長い。

### 開口を通れるか（実測）

胴の表面が床面を横切るあいだ、口の中心からどれだけ外へ出るか＝**要る開口半径**。

| 弦 | 口での傾き | 頭 (r 0.72) | 最太 (r 0.62) | 余裕（開口 2.20 m） |
|---|---|---|---|---|
| 8.00 m | 78.6度 | 0.99 m | 0.84 m | +1.21 m |
| 8.09 m | 77.4度 | 1.01 m | 0.86 m | +1.19 m |
| 8.75 m | 68.0度 | 1.21 m | 1.02 m | +0.99 m |
| 9.89 m | 48.9度 | **1.86 m** | 1.56 m | **+0.34 m** |
| 9.91 m | 48.5度 | 1.88 m | 1.57 m | +0.32 m |

> **一番きついのは «浅い» 経路の方。** 急な経路（内輪どうし 8.00 m）は 78 度でほぼ
> 真下へ抜けるので足跡が小さく、外輪どうし（9.89 m）は 48.9 度で入るぶん断面が
> 進行方向へ伸びて 1.86 m 要る。直感と逆なので、口を小さくするときはここを見ること。

羽は半径 2.18 m の半円で、滑り 2.53 m。平らな辺が中心から 2.53 m まで退くので
開口 2.20 m は完全に空く。沈み 0.30 m で羽は y = −0.32 … −0.52、床スラブ
（−0.60 … 0.00）の厚みの中に収まったまま外へ逃げる。

### 出入りのタイミング（実測して直した分）

寸法は通っていたが、**「いつ開いていつ閉じるか」の方に 3 つ穴があった。**

| | 症状 | 直した内容 |
|---|---|---|
| 登場 | 予兆の 0.6 秒のあいだ、**頭が閉じたままの羽を突き抜けていた** | `Emerge Depth` を 3.5 m へ。頭の当たりは関節から進行方向へ 1.9 m あるので、その分を見込む必要がある（浅い経路では 3.0 m が下限。2.5 m でも頭の天面が +0.05 m 出る） |
| 薙ぎ | 薙ぎきる先が入る口を越えていて、**「水平に薙ぐ」はずの手が「頭から潜る」**になっていた | 目標を `Length − 0.6 m` で頭打ちに |
| 撃破 | 死んだ瞬間に全部の口を閉じていたが、羽は 1.2 秒・胴が沈むのは 10 秒以上。**まだ出ている胴を羽が通り抜けて閉じていた** | 沈みきってから閉じる（`State::Dead` の中で 1 度だけ） |

問題の無かった経路も確認済み:

- **登場**は両方の口が開ききるまで胴を出さない（`State::Emerge` が `IsOpen` を待つ）
- **潜行**で出てきた側の口を閉じるのは、尾がその口を抜けた後（頭が `Length + 胴長` に着いた時点で、尾はちょうど入る側の口にいる）
- **突き上げ**で開けた口は、それが `from` / `to` と同じなら閉じない（渡っている胴の上で羽が戻らない）
- **蠕動**は両端で振幅 0 に落ちるので、口の真上で胴が縁を舐めない

### 開口の作り込み

- **段ごとに別の音**（`ReportStages`）。歯が外れる（`kImpactLight`）／羽が滑り出す（`kImpactDebris`）／閉じ切って噛む（`kImpactMid`）。開くまでの 1.2 秒はほぼ全部が予兆で、画面の外の口でも「いまどこまで進んだか」が耳で読める必要がある
- **滑り出しの 1 度だけ埃**（`PlayGroundDust` を法線の両側へ）。開いている間ずっと吹くと「煙が出ている穴」になる
- **カラーの回転は隣どうしで逆向き**。16 個が同じ向きに回ると「1 つの仕掛けのコピー」に見える
- **待機中の縁も薄く灯る**（Glow idle 1.2 → Glow 6.0）。0 にすると予兆の瞬間まで「どこが開きうるか」が分からない。6.0 だけがブルームのしきい値 4.0 を越える

### 未実装

**囲い込み**と**締め上げ**。あの 2 手は胴を複数の口から同時に出して輪にするもので、
1 本の円弧では表せない（`SerpentPathComponent` に輪の経路を足すところからになる）。
`SerpentAiComponent` は残り 4 手 ─ 突き上げ・薙ぎ・頭の突進・潜行 ─ を実装している。

予兆も、口の縁の発光（`SerpentApertureComponent`）とデバッグ描画の円/線までで、
`BossTelegraphComponent` 相当のデカール表示は入れていない
（あれは `BossAiComponent` を同じオブジェクトに要求する）。

---

## いまのシーンの状態

`Stage_02.scene` は結線済み。

- `Boss02_Arena_Map` の `scale` を **100 → 1**（取り込みの unit scale がルートに残っていた。
  100 のままだと半径 2200 m のアリーナになる）
- `COL_Shutter_<口>` 16 枚に `MeshCollider`（閉じている間の床。無いと 16 口が最初から
  落とし穴になる）
- `Boss02`: tag `Enemy` / `RigidBody`(static) / `PolarityTarget`(anchor + selfDriven) /
  `EnemyHealth`(HP 990) / `Animator`(externalPose) / `Serpent*` 7 本
- `Boss02_Arena_Map`: `SerpentApertureComponent`
- `Enviorment/ArenaLights` に 27 灯（下の「ライティング」参照）
- `MainCamera` の Boss 名を `Boss02` へ / `GameFlow` の勝利条件を `Boss02` へ
- `ArenaHazard` を**無効化**。中央 6 m を焼く手はボス1の «コア» の設計で、
  蛇は本体の原点がアリーナ中心にあるため、armed になった瞬間に自分が落ちる

---

## ライティング

寸法は書き出し済みメッシュの実測（baked `.fzasset` の頂点）。**ここは推測せずに測ること** ─
ノードの Transform は全部原点なので、Inspector を見ても分からない。

| | 高さ | 半径 |
|---|---|---|
| 床 / 床下 | 0.00 / −7.40 | 22.50 / 22.00 |
| 壁の光る帯（下） | **5.62 – 5.90** | 21.76 – 21.98 |
| 警告灯 | 5.69 – 6.11 | 21.73 – 21.95 |
| 壁の光る帯（上） | **9.90 – 10.18** | 21.76 – 21.98 |
| 壁の内側 | 〜12.50 | 22.00 |
| 天井 | 15.60 – 16.30 | 〜22.90 |

### 灯り（`Enviorment/ArenaLights`・27 灯）

| 名前 | 数 | 位置 | 種別 | 強度 |
|---|---|---|---|---|
| `TubeLower_00..09` | 10 | r 21.0 / y **5.76** | Tube（長さ 12・半径 0.12） | 16 |
| `TubeUpper_00..09` | 10 | r 21.0 / y **10.04** | Tube（同上） | 9 |
| `TrussSpot_00..05` | 6 | r 12.0 / y 13.4 | Spot（38°/60°・下 45 度・内向き） | 110 |
| `KeyDown_00` | 1 | 中央 / y 15.0 | Spot（40°/58°・真下） | 140 |

- 管球の高さは**光る帯の中心そのもの**。帯の上や下へずらすと «光源が壁に貼ってある»
  ではなく «壁が別の何かに照らされている» に見える。管の軸は Transform の Right なので、
  `yaw = 方位角` で輪の接線に乗る。
- **影を持つのは `KeyDown_00` の 1 灯だけ。** 27 灯すべてに影を持たせると ShadowPass が
  その数だけシーンを描き直す。読ませたいのは «胴が床のどこを跨いでいるか» の 1 点で、
  真上からの 1 灯があれば足りる。残りは塗りに徹してよい。
- トラスの 6 灯は方位を管球と 18 度ずらしてある。同じ列に並べると、帯と灯りが
  «1 本の光» に潰れて高さの手がかりが消える。

### 太陽と空

- `DirectionalLight`: 強度 **0.18** / **影なし**。屋根で完全に塞がれた部屋なので、
  太陽そのものは «漏れ» の量しか要らない。影を切ると屋根を無視した直射になるが、
  0.18 なら一様な薄い足しにしかならず、CSM のパスも 1 本省ける。
- `EnvironmentLight`: 0.55 / diffuse 0.30 / specular 0.35。閉じた鋼の部屋なので、
  空の IBL をそのまま浴びると屋外の明るさになる。
- フォグ: `fogSource` を **Atmosphere → Exponential** へ。距離 18 m から掛け始め
  （density 0.014 / color `(0.09, 0.10, 0.13)`）。
  > **`fogFar` は «霧が始まる距離»** で、名前に反して «不透明になる距離» ではない
  > （`Composite.hlsl`: `dist = max(linDepth - fogFar, 0)`）。既定の 80 m は
  > アリーナ（差し渡し 44 m）の外側なので、それまで霧は 1 度も掛かっていなかった。

### 発光マテリアル

取り込み直後の `.mat` には `emissiveColor` / `emissiveScale` が無く、
`emissiveScale` の既定は 0 ─ つまり**「光る帯」が 1 つも光っていなかった**。
（ボス1 の `M_AR_WarnLight` / `M_AR_CoreRing` は Editor で同じ値を入れてある。）

| 材質 | 色 | scale | 何を照らすか |
|---|---|---|---|
| `M_AR_StripGlow` | 冷白 `(0.62, 0.74, 0.95)` | 2.4 | 壁の 2 本の帯 |
| `M_AR_WarnLight` | 琥珀 `(1.00, 0.45, 0.10)` | 5.0 | 壁の警告灯 |
| `M_AR_CoreRing` | 琥珀 `(1.00, 0.62, 0.20)` | 2.0 | 床の 2 重の輪（口の輪と同じ半径） |
| `M_AR_RimGlow` | 琥珀 `(1.00, 0.62, 0.16)` | 1.2 | 開口の縁（待機時。予兆で 6.0 まで上がる） |
| `M_E_Ring_Idle` | 琥珀 `(1.00, 0.58, 0.14)` | 2.2 | 蛇の帯。«電気が通っている» の中立色 |

> **ブルームのしきい値は `Default.fzdata` で 4.0。** 帯（面積が大きい）はその下に置いて
> «明るい» だけを出し、**予兆だけがしきい値を越えて滲む**ようにしてある。
> `SerpentApertureComponent` の `Glow (idle) 1.2 → Glow 6.0` がその段差。
>
> **赤・青・緑は場に使わない**（12.2）。壁が赤や青に光ると、節に乗った極と読み違える。
> 場の色は琥珀と冷白だけに寄せてある。

> **注意: これらは `Library/Baked/<guid>/materials/` の `.mat`。**
> FBX を取り込み直すと `MatSubExporter` が無条件に上書きするので消える
> （ボス1 の材質も同じ条件で残っているだけ）。アリーナを書き出し直したら入れ直すこと。

### まだ触っていない

- 蛇の装甲（`M_E_ArmorGrey` / `M_E_MetalDark`）は `metallic = 0`。金属にすると
  27 灯のハイライトが胴を舐めて «機械» に見えるが、ボスの見た目そのものが変わるので
  ライティングの一環としては入れていない。
