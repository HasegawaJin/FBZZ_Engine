# .animcontroller / .mask の作り方

このフォルダのアセットは TOML のテキスト。Editor の Animator パネルからも作れるが、
手で書くほうが速い場面が多いので、**両形式の正本となる規則をここにまとめる**。

実装の正本はエンジン側:

| 形式 | 読み書き | 実行 |
|------|----------|------|
| `.animcontroller` | `Engine/src/Asset/AnimatorControllerAsset.cpp` | `Engine/src/Scene/Systems/AnimatorSystem.cpp` |
| `.mask` | `Engine/src/Asset/AvatarMaskAsset.cpp` | 同上 (`EnsureMaskLoaded` / `LayerBoneWeight`) |

書式で迷ったら、**Editor で一度開いて保存し直す**のが確実。`Save` は全キーを既定値込みで
書き出すので、手書きで省いたキーがどう埋まるかがそのまま読める。

---

## 1. `.mask` — どの骨に効くか

```toml
[mask]
version = 1
name = 'M_UpperBody'
default_include = false
skeleton_source = 'guid:<Player.fbx の guid>|Assets/Models/Player/Player.fbx'
skeleton_source_signature = ''

[[entries]]
bone = 'Spine'
include_children = true
blend_depth = 0
weight = 1.0
```

- `default_include` はどのエントリにも当たらなかった骨の重み (`false` = 0)。
  **「全身に効かせたい」なら `.mask` を指定しない** (レイヤーの `maskPath` を空にする)。
  空マスクと `default_include = true` は結果が同じだが、前者は読み込み自体が起きない。
- `bone` は **ボーン名だけでも、ルートからのパスでもよい**。名前だけで書くと、
  骨格が違うキャラでも同じマスクが使い回せる。GreenWare は全部この形。
- `include_children = true` はその骨の配下すべて。深いエントリほど強い
  (`EntrySpecificity` = パス階層数 × 1000 − 深さ) ので、
  「腕全体 0 → 手だけ 1」は**書く順番に関係なく**手が勝つ。
- `blend_depth` は境界のなだらかさ。`0` で即時、`2` なら 3 階層かけて `weight` まで立ち上がる。
  Spine のような境目で姿勢が折れるときに使う。

### 落とし穴

**読めない `.mask` は全身 0 になる。** パースに失敗した / パスが解決できないと
`FBZZ_LOG_ERROR("avatar mask load failed")` を出してレイヤーごと黙る。
以前は 1.0 (全身) に落ちていたが、それだと「上半身だけのはずのレイヤーが全身を上書きする」
という別の症状に化けるため、**指定したマスクが読めない間は効かせない**方針になっている。
モーションが 1 コマも出ないときは、まずログでこの行を探す。

**骨格ルートの上の飾りノードには当たらない。** Player なら実体は
`RootNode/PlayerRig/Root/Hips/...` で、`bone = 'Root'` は `Root` 配下にしか効かない。
`RootNode` と `PlayerRig` は重み 0 のままだが、これらはアニメーションされないので問題ない。

現行の Player 用 3 枚 (54 骨の骨格に対して):

| ファイル | エントリ | 骨数 |
|---|---|---|
| `Player_Base.mask` | `Root` 配下 | 54 (実質全身) |
| `M_UpperBody.mask` | `Spine` 配下 | 44 |
| `M_Arms.mask` | `LeftShoulder` / `RightShoulder` 配下 | 39 |

---

## 2. `.animcontroller` — ステートマシン

ルートに `version` / `defaultStateName` / `baseLayerMaskPath` があり、あとは
`[[states]]` `[[anyStateTransitions]]` `[[parameters]]` `[[layers]]` `[editorLayout]`。
**TOML なので順序は自由**だが、読む人のためにこの順で書く。

### 列挙値

| キー | 値 |
|---|---|
| `parameters.type` | Float 0 / Int 1 / Bool 2 / Trigger 3 |
| `states.mode` | Clip 0 / BlendTree1D 1 / BlendTree2D 2 |
| `conditions.op` | Greater 0 / Less 1 / Equal 2 / NotEqual 3 / True 4 / False 5 |
| `layers.mode` | Override 0 / Additive 1 |
| `blendTree2D.type` | SimpleDirectional 0 / FreeformCartesian 1 |

`True` / `False` は Bool と Trigger 専用、比較 4 種は Float / Int 専用。
型と op が噛み合っていない条件は**黙って false になる**ので、遷移が起きない原因になる。

### 省略したキーの既定値に注意

`ReadState` / `ReadTransition` は `value_or` で埋めるため、**省くと危険なキーがある**:

- `loop` は省略すると **true**。一撃モーションで省くと永久に終わらない。
- `ikWeight` は省略すると **1.0**。Blender で両手拘束・接地を作り込んだクリップに
  ゲーム側 IK が重なって手足が飛ぶ。GreenWare の Player は全ステート `0.0`。
- `transitionDuration` は省略すると **0.25**、`fixedDuration` は **true**。
- `conditions` が空で `hasExitTime = false` の遷移は**無効定義**として一生発火しない。

### クリップ参照 (`sourcePath`)

`.animcontroller` のステートは、**取り込み済みの `.anim` を導出 guid で**指す:

```toml
sourcePath = 'guid:8013f1b0c65116d85912a864d3f1019e|Library/Baked/652cf49f63ec40a8b4e3e3f5c80072f8/anims/Idle.anim'
```

- `|` の左が権威、右は人と grep のためのヒント。
- ディレクトリ名 `652cf4...` は**原本 FBX の guid** (`Idle.fbx.meta`)。
- 前置の `8013f1...` は `AssetDatabase::DeriveGuid(<FBX の guid>, "anims/Idle.anim")` の計算結果。
  FNV-1a を offset basis 違いで 2 本回して 128 bit にしたもので、
  **どこにも文字列として存在しない**。手で作れないので、生成スクリプトで計算するか
  Editor に書かせる。決定論的なので、Library を消して再インポートしても同じ値に戻る。
- `.fbx` を直接指してはいけない。`.fzasset` に clips は入っていないので、
  `LoadClips` が「クリップ 0 本のモデル」を読んで終わる (無音・ログなし)。

**スクリプト側のフィールド (`PlaySlot` に渡すパス) は別の書き方をする。** 詳細は下の Slot の節。

### AnyState と State 側の遷移

評価順は `AnimatorSystem::UpdateStateMachineScoped` にある通り:

1. 遷移中 (ブレンド中) は**新しい遷移を一切見ない**。
2. まず**現ステートの `transitions`** を上から順に見て、最初に成立した 1 本で確定。
3. **どれも成立しなかったときだけ** `anyStateTransitions` を上から順に見る。

この順序が設計を決める。**「流れ」は State 側、「割り込み」は AnyState** に置く。
同じ遷移を両方に書かない。Player の場合:

| 置き場所 | 中身 |
|---|---|
| State 側 | Locomotion → RunStop / ToBlocking、一撃モーション → Locomotion か FallLoop、FallLoop → Land、ガード 3 種の行き来、終局 3 種 → Reset |
| AnyState | Death / Defeat / Victory / Hit / GuardHit / Dodge / Jump / 攻撃 6 本 |

以前は Death・Victory・Hit への遷移を **20 ステート全部に複製**していて、
per-state 遷移だけで 120 本あった。AnyState へ寄せて 35 本 + 13 本になっている。

#### AnyState の 3 つの性質

**自分自身へは遷移しない。** `tr.toStateName == currentStateName` は無条件でスキップされる。
継続条件で毎フレーム自己遷移して再生位置が 0 に戻り続ける事故を防ぐためだが、
副作用として「Hit 中にもう一度 Hit」は AnyState では出せない (それは Slot の仕事)。

**Trigger は消費されるまで消えない。** `ConsumeTriggers` は**遷移が成立したときだけ**
条件に使われた Trigger を false へ戻す。1 フレームで自動的に落ちるわけではないので、
遷移できない状況で立てた Trigger は**後から効く**。

**AnyState は全ステートに等しく効く。** 除外リストは無い。
だから「Reset が来るまで保持したいステート」は **Bool の門で守る**しかない。
Player は `IsDead` がそれで、`PlayerComponent` が毎フレーム `!IsAlive()` を書き、
AnyState の割り込み側 (Hit / GuardHit / Dodge / Jump / 攻撃) に `IsDead == False` を付けている。
これが無いと、撃破時に残っていた Trigger が Death を蹴り出して起き上がる。

### レイヤーと Slot

`[[layers]]` は Base Layer の**上**に重なる。ステートマシンを持たせることもできるが、
GreenWare の Player は 2 枚とも**空のステートマシン + Slot 専用**:

```toml
[[layers]]
name = 'Attack'
weight = 0.0            # BladeComponent が Slot の重みから毎フレーム駆動する
mode = 0                # Override
enabled = true
maskPath = 'guid:...|Assets/Animation/Player/Player_Base.mask'
defaultStateName = ''
states = []
anyStateTransitions = []
retargetMappings = []

[layers.additiveReference]
sourcePath = ''
clipName = ''
time = 0.0

[layers.slot]
fadeInDuration = 0.15
fadeOutDuration = 0.15
```

- **ステートも Slot も空のレイヤーは完全にスキップされる**ので、`weight = 1.0` で置いておいても害はない。
- `weight` を誰かが駆動しないレイヤーは**一生出ない**。`Attack` は `BladeComponent`、
  という具合に「誰が重みを書くか」を必ず決めてから足す。
- **Slot はレイヤーに 1 本だけ**。同じレイヤーへ 2 系統の演出を流すと後から来たほうが前を消す。
- `mode = 1` (Additive) にするなら `additiveReference` に基準ポーズを入れる。
  入れないと «全身ポーズをそのまま加算» になって骨が二重に回る。

#### Slot で鳴らすクリップは «登録» が要る

`animator.PlaySlot(layer, source, clip, ...)` は、そのクリップが `animator.clips` に
**載っていなければ無音で終わる**。`UpdateLayerSlot` が `slot.active = false` にするだけで
警告もログも出ない。`AnimatorSystem::LoadClips` が集めるのは次の 3 つだけ:

1. 各ステート (BlendTree の motion 含む) の `sourcePath`
2. レイヤーの `additiveReference`
3. `animator.externalClipSources`

Player の攻撃・被弾クリップが Slot で鳴るのは、**Base Layer に同名ステートがあって ① で載っているから**。
Controller の `Slash01` 〜 `UnderSlashandUpperSlash` と `Hit` のステートは、
Trigger で入ることを狙ったものではなく**クリップ登録と手動確認のための入口**でもある。
ステートを消すときは、そのクリップを Slot で鳴らしている側が無音にならないか必ず確認する。

新しい演出専用クリップを足すなら、鳴らす側が
`scene.GetComponent<AnimatorComponent>()->externalClipSources` へパスを積み、
`clipsLoaded = false` で読み直させる (`PlayerClimbComponent::RegisterClips` が実例)。
**この ③ のパスだけは `guid:` を前置しない論理パス** `Assets/<dir>/<Name>/anims/<Clip>.anim`
で書く。`guid:` を付けると guid が勝ってパスのヒントが捨てられ、FBX へ戻ってしまう。

Slot が鳴っているかは `GetSlotWeight()` を読むのが早い。0 のままなら畳まれている。

---

## 3. 手で書いたあとの確認

Editor を立ち上げる前に、テキストの段階で落とせる事故が多い。

1. **TOML として読めるか** — `py -c "import tomllib; tomllib.load(open('X.animcontroller','rb'))"`
2. **`toStateName` が全部実在するか** — 存在しないステートへの遷移は黙って無視される。
3. **`conditions.paramName` が `[[parameters]]` にあるか** — 無いパラメーターの条件は常に false。
4. **パラメーター型と `op` が噛み合っているか** — Bool/Trigger に比較 op、Float に True/False は効かない。
5. **`sourcePath` の `|` 右のファイルが実在するか** — FBX 未インポートなら `Library/Baked/...` が無い。
6. **`defaultStateName` が実在するか** — 無いと `states[0]` に落ちる。
7. **どのステートにも到達できるか** — AnyState 込みで辿って孤立ステートを洗う。
8. **`loop` と `ikWeight` を全ステートに書いたか** — 省略時の既定値が逆に振れる。

Player の Controller はこの 8 点を通した生成スクリプトから出している。
クリップの秒数・ループ・参照の正本は `Assets/Models/Player/Clips.csv`。

---

## 4. 関連

- `Assets/Animation/Player/README.md` — Player の Controller の中身
- `Assets/Models/Player/README.md` — 骨格・クリップ・書き出し元
- `Docs/design/player-export-controller.md` — 今の構成に決めた経緯
