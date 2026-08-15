# スクリプト記述体験の改善計画

調査日: 2026-06-28
対象: `Projects/Engine/include/Engine/Scene/Script.hpp` 系統 + `DemoGame/Assets/Scripts/` 全スクリプト

絶対条件（本計画の評価軸）:
1. **AI がコーディングしやすい**（少ない文脈・少ない暗黙ルールで正しく書ける）
2. **人間がコーディングしやすい**（補完が効く・ボイラープレートが少ない）
3. **ゲームがしっかり作れる**（オブジェクト参照・スクリプト間連携が型安全で堅牢）

---

## 0. 結論（TL;DR）

現状のスクリプトは「**機能は揃っているが、書く時の摩擦が大きい**」状態。摩擦の根は次の 4 つに集約される。

| # | 摩擦の根 | 影響する制約 |
|---|---------|------------|
| A | オブジェクト参照が「文字列名 + 手書き親階層探索」で、毎回 `ResolveOwner()` を再実装している | ③（最重要）, ②, ① |
| B | 1 スクリプト = **3 ファイル**（`.hpp` / `.cpp` スタブ / `.generated.hpp`）で、生成ファイルの更新漏れフットガンがある | ①, ② |
| C | `FBZZ_FIELD` が位置引数 + 表示名の重複で冗長 | ①, ② |
| D | アニメ状態・タグなどの「ゲーム語彙」が魔法文字列で 4 スクリプトに散在 | ③, ② |

推奨する優先順位（投資対効果順）:

1. **P1 — 参照型を第一級にする（`Ref<T>` + `FBZZ_REF`）** … ③を劇的に改善。インフラ（Inspector の D&D）は既に存在し、採用するだけで効果が大きい。
2. **P2 — `FBZZ_FIELD` の表示名を自動化し、`.generated.hpp` を廃止する** … ①②を劇的に改善。3 ファイル → 1 ファイル化の前提。
3. **P3 — 1 スクリプト = 1 ファイル化（`.cpp` スタブ / `.generated.hpp` の廃止）** … P2 の成果物。
4. **P4 — ゲーム語彙（アニメ状態等）を共有定数化** … ③④の地味だが効くクリーンアップ。
5. **P5（任意）— 型安全メッセージング** … スクリプト間連携の総仕上げ。

> 既に良い部分は**壊さない**。35 個のプロキシをメンバー直持ち（`particle.`, `trail.`, `animator.`）にして補完で全 API が出る設計は、AI・人間の双方に効いている最大の資産。本計画はこの上に乗せる。

---

## 1. 現状分析（証拠付き）

### 1-A. オブジェクト参照が文字列ベースで毎回手書き

デモの 9 スクリプトで `EntityRef` / `FBZZ_FIELD_REF`（=用意済みの型安全参照）の採用は **ゼロ**。代わりに全員が文字列名 + 親階層ウォークを手書きしている。

`SwordTrailComponent.hpp:105-121`, `WeaponHitboxComponent.hpp:99-116`, `EnemyControllerComponent.hpp:123-130` に **ほぼ同一の `ResolveOwner()/FindTarget()`** が重複:

```cpp
// 3 ファイルで重複している典型パターン
FBZZ_FIELD(std::string, ownerName, "Player", "Owner Name")   // 文字列で参照を持つ
...
GameObject* ResolveOwner() {
    if (!ownerName.empty())
        if (auto* o = scene.Find(ownerName)) return o;        // 名前で線形探索
    GameObject* cur = m_gameObject;
    while (cur) {                                             // 親をたどって Player/Enemy を探す
        if (cur->CompareTag("Player") || cur->name == "Player") return cur;
        cur = cur->GetParent();
    }
    return nullptr;
}
```

さらにスクリプト間連携も文字列の合成で行っている（`WeaponHitboxComponent.hpp:236`）:

```cpp
const std::string trailObjectName = m_owner->name + "_Sword_Trail";  // 命名規約に依存。壊れやすい
auto* trailObject = scene.Find(trailObjectName);
if (auto* swordTrail = trailObject->GetScript<SwordTrailComponent>()) ...
```

**問題点:**
- リネームすると黙って壊れる（コンパイルが通る）。
- 毎回キャッシュ無効化と再解決のボイラープレート（`m_owner`, `FindTarget()`）を書く。
- AI から見て「参照はどう持つのが正解か」が一意でない（`std::string` 名 / 親ウォーク / `scene.Find` の合成）。

**重要:** これを解決する土台はすでに完成している。
- `EntityRef`（`EntityRef.hpp`）と `IReflector::Field(EntityRef&)` は実装済み。
- Inspector はドラッグ&ドロップでの参照アサインを**既に実装済み**（`ImGuiReflector.hpp:73-105`、`FBZZ_HIERARCHY_ENTITY` ペイロードを受け取る）。
- つまり「使われていないだけ」。エルゴノミクスの薄いラッパーを足して、デモを移行すれば即効果が出る。

### 1-B. 1 スクリプト = 3 ファイル + 生成フットガン

`SwordTrailComponent` を作るのに必要なファイル:

| ファイル | 役割 |
|---------|------|
| `SwordTrailComponent.hpp` | クラス宣言 + `FBZZ_FIELD` + `#include "*.generated.hpp"` + `#ifndef *_IMPL` で実装本体 |
| `SwordTrailComponent.cpp` | **中身は実質 4 行**の TU スタブ（`*_IMPL` を定義して `.hpp` を include するだけ、`SwordTrailComponent.cpp:1-7`） |
| `SwordTrailComponent.generated.hpp` | `ScriptCodeGen`（FHT）が `FBZZ_FIELD` を解析して `Reflect()` を生成（`SwordTrailComponent.generated.hpp`） |

**問題点:**
- フィールドを 1 個足すたびに `.generated.hpp` を再生成しないと、Inspector とシリアライズが**黙って**古いままになる（コンパイルは通る）。AI が最も踏みやすい罠。
- `.hpp` が「宣言 → generated include → `_IMPL` 実装」と 1 ファイル内で 3 層に折り重なっており、新規読者（人間/AI）が構造を把握しづらい。
- `.cpp` スタブはほぼ無意味な定型（リンク都合）なのに、新規スクリプトのたびに必ず付いてくる。

### 1-C. `FBZZ_FIELD` の冗長さ

```cpp
FBZZ_FIELD_RANGE(float, swingStartTime, 0.18f, "Swing Start Time", 0.0f, 1.0f)
//               型      変数名         既定値  表示名（変数名の再掲）  min   max
```

- 表示名 `"Swing Start Time"` は `swingStartTime` の機械的な整形にすぎず、**ほぼ常に重複**。
- 位置引数が 6 個あり、min/max の順序ミスや型/既定値の取り違えを AI がやりがち。
- マクロが `FBZZ_FIELD` / `FBZZ_FIELD_RANGE` / `FBZZ_FIELD_ENUM` / `FBZZ_FIELD_REF` / `FBZZ_COMPUTED` と分岐し、どれを使うべきかの判断が必要。

### 1-D. ゲーム語彙が魔法文字列で散在

戦闘状態の判定が 4 スクリプトに重複している:

```cpp
// PlayerController / Enemy / SwordTrail / WeaponHitbox に同じ文字列が散る
animator.IsInState("Slash_01") || animator.IsInState("Slash_02") ||
animator.IsInState("Slash_03") || animator.IsInState("CrouchSlash")
```

タイプミスは黙って「常に false」になり、デバッグが難しい。アニメ状態を 1 つ追加すると全スクリプトを手で直す必要がある。

---

## 2. 3 制約でのスコアリング（現状）

| 観点 | 現状 | 主因 |
|------|:---:|------|
| ① AI が書きやすい | 4/10 | 3 ファイル + 生成漏れフットガン、参照の正解が一意でない |
| ② 人間が書きやすい | 5/10 | 表示名重複、ボイラープレート（Resolve/キャッシュ）、補完は効く（プロキシは◎） |
| ③ ゲームが作れる | 6/10 | 機能は十分だが参照が文字列頼みで壊れやすい・連携が脆い |

---

## 3. 提案

### P1 — 参照型を第一級にする（最優先 / ③へ最大効果）

**狙い:** 文字列名 + `ResolveOwner()` を、宣言的な型安全参照に置き換える。Inspector の D&D 土台は既存なので**追加実装は薄い**。

#### 3-1. ランタイム側: 自動解決する `Ref<T>` ハンドル

`EntityRef` を「解決済み GameObject* をキャッシュしつつ毎回安全に再解決する」薄いハンドルで包む。`T` を指定すると Script/Component への型付きアクセスになる。

```cpp
// Engine/Scene/Ref.hpp（新規・概念図）
template<typename T = GameObject>
struct Ref {
    EntityRef ref;                 // シリアライズ実体（= 既存 EntityRef）
    // 解決は scene プロキシ経由。無効なら nullptr。内部で 1 フレームキャッシュしてもよい。
    T* Get(const ScriptSceneProxy& scene) const;
    T* operator->() const;         // Script に bind 済みなら scene 不要で使える設計も可
    explicit operator bool() const;
};
```

#### 3-2. オーサリング側: `FBZZ_REF`

```cpp
// Before（SwordTrailComponent）: 文字列 + 20 行の ResolveOwner()
FBZZ_FIELD(std::string, ownerName, "Player", "Owner Name")
GameObject* ResolveOwner();           // 手書き・3 ファイルで重複

// After: 宣言 1 行。Inspector に D&D スロットが出る。手書き解決は不要。
FBZZ_REF(GameObject, owner, "Owner")              // 任意の GameObject を D&D
FBZZ_REF(SwordTrailComponent, swordTrail)         // 型付きスクリプト参照（文字列合成が消える）
```

使用側:

```cpp
// Before（WeaponHitbox の脆い文字列合成）
auto* trailObject = scene.Find(m_owner->name + "_Sword_Trail");
if (auto* t = trailObject->GetScript<SwordTrailComponent>()) t->PlayBloodSpray();

// After（型安全・リネーム耐性あり）
if (swordTrail) swordTrail->PlayBloodSpray();
```

**効果:**
- ③ リネームしても Inspector のアサインが ID で保持され壊れない。スクリプト間連携が型安全に。
- ② `ResolveOwner()` / `FindTarget()` / `m_owner` キャッシュのボイラープレートが消える。
- ① 「参照はどう持つか」が `FBZZ_REF` の 1 通りに収束。AI の迷いが消える。

**段階導入:** まず `Ref<T>` + `FBZZ_REF` を追加 → デモ 1 本（SwordTrail）を移行して検証 → 残りを順次移行。文字列 `ownerName` は当面併存可（移行猶予）。

> 補足: 「親階層から Player/Enemy を探す」挙動は、Inspector 未アサイン時のフォールバックとして `Ref<T>` 側に共通ヘルパー（例 `scene.FindInParents<Tag>()`）を 1 つ用意すれば、各スクリプトの重複ループを 1 行に畳める。

---

### P2 — `FBZZ_FIELD` の表示名自動化 + `.generated.hpp` 廃止（①②へ最大効果）

#### 3-3. 表示名を任意化（後方互換）

```cpp
// After: 表示名を省略すると変数名から自動整形（"swingStartTime" → "Swing Start Time"）
FBZZ_FIELD(float, swingStartTime, 0.18f)
FBZZ_FIELD(float, swingStartTime, 0.18f, "Custom Label")   // 必要時だけ明示（従来も維持）
```

属性（range/enum）は**チェーン属性**または**トレーリング修飾**で表現し、マクロ種別の分岐を減らす案:

```cpp
FBZZ_FIELD(float, swingStartTime, 0.18f) FBZZ_RANGE(0.0f, 1.0f)
```

#### 3-4. `.generated.hpp` を廃止し、`Reflect()` をマクロ自己登録で生成

**現状の本質的問題は「単一の真実が 2 箇所（`.hpp` の宣言と `.generated.hpp` の `Reflect`）に分裂し、外部ツール実行で同期している」こと。** これを **プリプロセッサだけで完結**させ、生成ファイルと FHT 実行を不要にする。

実装方針（X-macro / 自己登録のいずれか）:

- **案 a（X-macro）:** 各スクリプトがフィールドを `XXX_FIELDS.inl` 的なリストに 1 回だけ書き、`Reflect()` と「メンバー宣言」の両方をマクロ展開で生成。真実は 1 箇所。
- **案 b（記述子テーブル）:** `FBZZ_FIELD` を「メンバー宣言 + `static` 記述子配列への登録」に展開し、基底 `Script::Reflect()` が記述子配列を走査。スクリプト側に `Reflect()` 実装も生成ファイルも不要。

いずれでも **AI/人間は「フィールドを 1 行足すだけ」で Inspector・シリアライズが自動追従**し、1-B の生成漏れフットガンが消滅する。

> トレードオフ: マクロ/テンプレートの実装は一度書くと複雑になる。複雑さを「エンジン基盤側に 1 回」閉じ込め、スクリプト作者には見せないのが要点。MSVC のマクロ展開で過去に苦労した経緯（`Script.hpp:101-108` のコメント）があるため、案 b（クラス外 `Reflect` を残しつつ記述子で駆動）の方が安全側。

---

### P3 — 1 スクリプト = 1 ファイル化

P2 で `.generated.hpp` が消えれば、残る `.cpp` スタブ（`SwordTrailComponent.cpp:1-7` の 4 行）も統合できる。

**目標レイアウト:** スクリプトは `.hpp` 1 枚。宣言 + 実装をインラインで持ち、`_IMPL` ガードと生成 include を撤去。TU 都合（`Scene.hpp` 実体化）はテンプレートの登録側（`*ScriptsDll.cpp` / `GameMain.cpp`）か単一の集約 TU に寄せる。

**効果:** ① AI が 1 ファイルだけ読めば全文脈が揃う（最重要）。② 新規作成のファイル数 3→1。`Editor の "Create → C++ Script..."` の生成物も 1 枚に。

> 注意: ヘッダーオンリー化は同名スクリプト同士の inline 実装重複に注意。`inline` メソッド化 or 集約 TU で対処。互換のため当面は `.cpp` スタブ自動生成を残し、新規のみ 1 枚化する移行も可。

---

### P4 — ゲーム語彙の共有定数化

魔法文字列を 1 箇所に集約する。最小実装はプロジェクト共有ヘッダー:

```cpp
// DemoGame/Assets/Scripts/GameVocab.hpp（新規）
namespace sandbox::AnimState {
    inline constexpr const char* Slash01     = "Slash_01";
    inline constexpr const char* CrouchSlash = "CrouchSlash";
    // ...
}
// 「攻撃中か」を 1 箇所に集約するヘルパーも置く
bool IsSwingState(const ScriptAnimatorProxy&, GameObject* = nullptr);
```

**効果:** ③ アニメ状態追加が 1 箇所修正で済む。④ タイプミスがコンパイルエラー化（`constexpr`）。② 補完が効く。

> 発展: AnimatorController 側から状態名を**コード生成**して `AnimState` を吐けば、デザイナーの変更とコードが自動同期する（P2 の codegen 基盤を再利用できる）。

---

### P5（任意）— 型安全メッセージング

`scene.Find(name)->GetScript<T>()->Method()` の連鎖を、型安全なイベント/メッセージで置き換える総仕上げ。

```cpp
// 例: 武器ヒット時に「血しぶき」を要求側が型で投げ、受け手が購読
events.Emit<SwordHitEvent>({ .attacker = owner, .point = info.contactPoint });
```

P1 で大半の連携は `Ref<T>` 直呼びに置き換わるため、P5 は「1 対多 / 疎結合が欲しい場面」だけの追加オプション。優先度は最低。

---

## 4. 移行ステップと工数感

| フェーズ | 内容 | 規模感 | リスク |
|--------|------|:---:|:---:|
| P1-a | `Ref<T>` + `FBZZ_REF` を Engine に追加（Inspector D&D は既存流用） | 小 | 低 |
| P1-b | デモ 1 本（SwordTrail）を移行・検証 → 残りへ展開 | 中 | 低 |
| P2-a | `FBZZ_FIELD` 表示名の任意化（後方互換） | 小 | 低 |
| P2-b | `Reflect()` 自己登録化 → `.generated.hpp` 撤廃（案 b 推奨） | 中〜大 | 中（MSVC マクロ） |
| P3 | `.cpp` スタブ撤廃 / 1 ファイル化、Create テンプレ更新 | 中 | 中 |
| P4 | `GameVocab.hpp` 集約 + デモ置換 | 小 | 低 |
| P5 | 型安全メッセージング（任意） | 中 | 中 |

**推奨着手順:** P1 → P2 → P3 →（P4 はいつでも）→ P5。
P1 は土台が揃っているため、最小投資で③（ゲームが作れる）の体感が最も上がる。P2/P3 で①②（AI/人間の書きやすさ）が段違いになる。

---

## 5. 改善後の到達イメージ（Before / After 全体像）

```cpp
// ===== Before: SwordTrailComponent（3 ファイル・文字列参照・generated 同期必須） =====
FBZZ_FIELD(std::string, ownerName, "Player", "Owner Name")
GameObject* ResolveOwner();              // 20 行の手書き、3 スクリプトで重複
GameObject* m_owner = nullptr;           // 手動キャッシュ
// + SwordTrailComponent.cpp(スタブ) + SwordTrailComponent.generated.hpp(要再生成)

// ===== After: 1 ファイル・型安全参照・自動 Reflect =====
FBZZ_REF(GameObject, owner, "Owner")     // Inspector に D&D スロット。解決もキャッシュも内蔵
FBZZ_FIELD(float, swingStartTime, 0.18f) FBZZ_RANGE(0, 1)   // 表示名は自動
// generated.hpp なし / .cpp スタブなし
```

| 観点 | 現状 | 改善後（目標） |
|------|:---:|:---:|
| ① AI が書きやすい | 4/10 | 9/10 |
| ② 人間が書きやすい | 5/10 | 9/10 |
| ③ ゲームが作れる | 6/10 | 9/10 |

---

## 6. 制約遵守チェック（プロジェクト規約）

- ✅ GLM / Bullet 等の外部ライブラリ不使用（自作 `Math` / `EntityRef` の上に構築）。
- ✅ `DX11Renderer` ダウンキャストなし（本計画は Scene/Script レイヤー内で完結）。
- ✅ `new` / `delete` 直接使用なし（`Ref<T>` は値型・`EntityID` 参照、所有権を持たない）。
- ✅ `throw` / 例外なし（参照解決は nullptr フォールバック）。
- ✅ スクリプトは引き続き `ScriptProxy` 経由でのみ Engine 実装へアクセス（`Ref<T>::Get` も `ScriptSceneProxy` 経由）。
- ✅ コメントを厚く残す（ポートフォリオ方針）。

---

## 7. オープンな論点（着手前に決めること）

1. **P2 の生成方式**: 案 a（X-macro）か案 b（記述子テーブル）か。MSVC マクロ展開の安定性を踏まえると案 b 推奨だが、要 PoC。
2. **`Ref<T>` の解決タイミング**: 毎回解決 / 1 フレームキャッシュ / `OnStart` で 1 回。シーン再読み込みとの整合を決める。
3. **識別子の設計**: 文字列参照は現行の `EntityID` / `Ref<T>` 解決へ統一する。
4. **1 ファイル化の TU 戦略**: `inline` メソッド化 vs 集約 TU。ビルド時間への影響を計測。
</content>
</invoke>
