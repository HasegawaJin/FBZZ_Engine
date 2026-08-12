# Reflection: 入れ子オブジェクトと構造体配列

`IReflector` に「名前付きスコープ」と「構造体の配列」を追加し、
その基盤の上で `PostProcessProfile` を `.fzdata` (DataAsset) として実装する。

## 背景 — 何が既にあり、何が無いのか

調査の結果、リフレクション基盤は当初の想定よりはるかに揃っていた。

### 実装済み

| 機能 | 実装箇所 |
|------|---------|
| `FBZZ_OBJECT_FIELD(Type, Name, Display)` | `Script.hpp:493` |
| `IReflector::ObjectField(name, IScriptSerializable&)` | `Script.hpp:221` (既定はフラット展開) |
| シーン保存の入れ子テーブル化 | `SceneSerializer.cpp:526` / `:729` |
| Inspector の折りたたみ表示 | `ImGuiReflector.hpp:685` |
| `FBZZ_FIXED_ARRAY_FIELD` (`std::array<T,N>`) | `Script.hpp:481` |
| `FBZZ_LIST_FIELD` (スカラー / Vector / EntityRef の配列) | `Script.hpp:467` |
| `.fzdata` の `.meta` / GUID / リネーム追随 | `AssetDatabase.cpp:148` (拡張子リストに登録済み) |
| `DataAssetRef` の Inspector D&D スロット | `Script.hpp:195` / `FBZZ_ASSET` マクロ |
| `DataAssetRegistry` の共有実体キャッシュ | `DataAssetRegistry.cpp` |

### 欠けている 2 点

**1. `DataAssetRegistry` のリフレクタが入れ子に対応していない**

`DataAssetRegistry.cpp:69-111` の `TomlWriteReflector` / `TomlReadReflector` は
スカラーと Vector/Quaternion しか override していない。`ObjectField` は
`IReflector` の既定実装 (名前を捨てて同じテーブルへフラット展開) に落ちる。

結果、`bloom.intensity` と `sharpen.intensity` が**同じ `intensity` キーへ衝突する**。
`SceneSerializer` には正しい入れ子実装があるのに、DataAsset 側だけ取り残されている。

**2. 構造体の配列 (`std::vector<StructType>`) がどこにも無い**

`ListField` のオーバーロードはスカラー / Vector / `EntityRef` のみ。
`std::vector<CustomPostProcessSettings>` のような「構造体の配列」を表現する手段が
`IReflector` 全体に存在しない。

## 設計方針

- **既存の呼び出しを一切壊さない。** 追加する仮想関数はすべて既定実装を持ち、
  未対応のリフレクタは従来どおりの挙動 (フラット展開 / 無視) を維持する。
- **`IScriptSerializable` の継承を強制しない。**
- **仮想関数はクラス末尾に追記し、`kReflectionAbiVersion` をインクリメントする。**

### WHY: `ObjectField` をそのまま使わないのか

`ObjectField` は入れ子の型が `scene::IScriptSerializable` を継承していることを要求する。
今回反映したい `BloomSettings` / `FogSettings` などは
`Engine/Renderer/RenderSettings.hpp` に住むレンダラーの純粋なデータ構造体である。

これらに Scene 層のインターフェースを継承させると、`RenderSettings.hpp` が
`Script.hpp` を include することになる。`Script.hpp` は `Scene` 層をまるごと引き込む
巨大ヘッダであり、**レンダラーがシーンに依存する**という逆流を生む。
`Docs/conventions` の依存方向にも反する。

そこで、継承を要求しない**スコープ対** (`BeginObject` / `EndObject`) を追加する。
これなら任意の平坦な構造体を、自由関数として書いた `Reflect` ヘルパーで反映できる。

## 追加する API

`IReflector` の末尾に以下を追加する。

```cpp
// ── 名前付きスコープ ──────────────────────────────────────────────────────
// BeginObject / EndObject で挟んだ範囲を 1 つの入れ子オブジェクトとして扱う。
// 既定実装は何もしない = 従来どおり親と同じ階層へフラット展開される
// (後方互換。未対応リフレクタでも動作は変わらない)。
//
// WHY 継承を要求しないか: 反映したい構造体がレンダラー層に住むため、
//     Scene 層のインターフェース継承を強制すると依存方向が逆流する。
virtual void BeginObject(const char* name) { (void)name; }
virtual void EndObject() {}

// ── 構造体の配列 ─────────────────────────────────────────────────────────
// 現在の要素数を渡し、リフレクタが決めた新しい要素数を返す。
// 読み込みリフレクタは保存されていた要素数を、Inspector は
// ユーザーが Add / Remove した後の要素数を返す。書き込みリフレクタは
// 受け取った値をそのまま返す。
//
// 呼び出し側は戻り値で vector を resize してから、要素ごとに
// BeginObjectElement / EndObjectElement で挟んで反映する。
//
// WHY 「戻り値で要素数を返す」形にするか:
//   読み込み・UI 編集・書き込みの 3 方向すべてで要素数の変更が起こりうる。
//   コールバックを渡す設計にすると DLL 境界を越える std::function が増え、
//   ScriptDllAbi の互換管理が複雑になる。戻り値なら vtable に関数を 4 つ足すだけで済む。
[[nodiscard]] virtual size_t BeginObjectList(const char* name, size_t count)
{
    (void)name;
    return count;   // 既定: 要素数を変えない
}
virtual void BeginObjectElement(size_t index) { (void)index; }
virtual void EndObjectElement() {}
virtual void EndObjectList() {}
```

### 呼び出し側の書き方

```cpp
// 入れ子オブジェクト
r.BeginObject("bloom");
r.Field("enabled", settings.bloom.enabled);
r.FloatRange("intensity", settings.bloom.intensity, 0.0f, 5.0f);
r.EndObject();

// 構造体の配列
const size_t count = r.BeginObjectList("customEffects", settings.customEffects.size());
settings.customEffects.resize(count);
for (size_t i = 0; i < count; ++i) {
    r.BeginObjectElement(i);
    ReflectCustomEffect(r, settings.customEffects[i]);
    r.EndObjectElement();
}
r.EndObjectList();
```

### 既存 `ObjectField` との関係

`ObjectField` の既定実装を `BeginObject` / `EndObject` で包む形に書き換える。

```cpp
virtual void ObjectField(const char* name, IScriptSerializable& value)
{
    BeginObject(name);
    value.Reflect(*this);
    EndObject();
}
```

これにより、`BeginObject` を実装したリフレクタは `ObjectField` の入れ子化も
自動的に手に入る。`SceneSerializer` / `ImGuiReflector` の既存 `ObjectField` override は
`BeginObject` 実装へ統合して重複を消す。

## 各リフレクタへの実装

`IReflector` の実装は現在 12 箇所ある。既定実装があるため**全部を直す必要はない**。

| リフレクタ | 対応 | 理由 |
|-----------|------|------|
| `SceneSerializer` TomlWrite / TomlRead | **必須** | 既存 `ObjectField` を `BeginObject` へ統合。テーブルスタックを持たせる |
| `DataAssetRegistry` TomlWrite / TomlRead | **必須** | 今回の主目的。`.fzdata` の入れ子対応 |
| `ImGuiReflector` | **必須** | 折りたたみ + 配列の Add/Remove UI |
| `ScriptSnapshot` Writer / Reader | **必須** | 未対応だとホットリロード時に入れ子データが消える |
| `JsonReflector` (Read/Write/Catalog) | 対応する | AI (MCP) からプロファイルを編集させるため |
| `AnimationPropertyReflector` | 既定のまま | アニメーション可能な float を拾う専用。入れ子は対象外 |
| `VFXAttributeReflector` | 既定のまま | 同上 |

### テーブルスタックの実装

TOML 系リフレクタは現在 `toml::table&` を 1 つだけ保持している。
`BeginObject` / `EndObject` を実装するため、スタックを持たせる。

```cpp
class TomlWriteReflector : public IReflector {
    // 現在の書き込み先。末尾が active。
    std::vector<toml::table*> m_stack;
    // BeginObject で作った一時テーブル。EndObject で親へ move する。
    std::vector<std::pair<std::string, toml::table>> m_pending;
};
```

**WHY 生ポインタのスタックか**: `toml::table` は move で内部ノードのアドレスが変わりうるため、
親テーブルへ挿入するのは子の構築が完全に終わった `EndObject` の時点に限る。
構築途中の子は `m_pending` が所有し、確定後に親へ移す。

## `kReflectionAbiVersion` の更新

`ScriptDllAbi.hpp:68` の `kReflectionAbiVersion` を **2 → 3** にする。

**WHY 必須か**: スクリプト DLL の `Reflect()` は vtable インデックスで仮想呼び出しする。
`IReflector` に仮想関数を追加すると、再ビルドされていない DLL は
署名が一致したまま vtable がズレてクラッシュする。
バージョンを上げれば stale な DLL は「Missing Script」として安全に拒否される。

**新しい仮想関数はクラス末尾に追記する。** 途中に挿入すると既存関数のインデックスまで
ずれ、ABI チェックをすり抜けた場合の被害が大きくなる。

---

# PostProcessProfile — `.fzdata` としての実装

## 型定義

```cpp
// Engine/include/Engine/Asset/PostProcessProfile.hpp
class PostProcessProfile final : public asset::DataAsset {
public:
    static constexpr const char* TYPE_NAME = "PostProcessProfile";

    const char* GetTypeName() const override { return TYPE_NAME; }
    void        Reflect(scene::IReflector& r) override;   // 手書き

    renderer::PostProcessSettings settings;
};
FBZZ_REGISTER_DATA_ASSET(PostProcessProfile);
```

**WHY `FBZZ_REFLECT` マクロ群を使わず `Reflect()` を手書きするか**:
`PostProcessSettings` はレンダラー側の既存構造体で、フィールド宣言を
`FBZZ_FIELD` マクロへ置き換えることはできない (レンダラーが Script.hpp に依存してしまう)。
手書きにすることで、各フィールドへ適正なレンジ・ツールチップ・カラーピッカー指定を
個別に与えられ、既存の `PostProcessInspectorWidgets` と同等の編集体験を保てる。

**WHY `Asset<T>` が使えるか**: `Asset<T>` が要求するのは `T::TYPE_NAME` のみ。
マクロを使わなくても満たせる。

## `Reflect()` の構成

`RenderSettings.hpp` の各サブ構造体ごとに自由関数の反映ヘルパーを用意する。

```cpp
// PostProcessProfile.cpp (無名名前空間)
void ReflectBloom(scene::IReflector& r, renderer::BloomSettings& v)
{
    r.Field("enabled", v.enabled);
    r.FloatRange("intensity", v.intensity, 0.0f, 5.0f);
    r.FloatRange("threshold", v.threshold, 0.0f, 4.0f);
    r.FloatRange("softKnee",  v.softKnee,  0.0f, 1.0f);
}
```

`float color[3]` は `math::Vector3` を経由し、カラーピッカーのヒントを付ける。

```cpp
// WHY 経由が要るか: IReflector は C 配列を直接扱えない。
//     一時 Vector3 へ写して反映し、書き戻す。
void ReflectColor3(scene::IReflector& r, const char* name, float (&color)[3])
{
    math::Vector3 value{ color[0], color[1], color[2] };
    r.SetFieldHint(scene::FieldHint::Color);
    r.Field(name, value);
    color[0] = value.x; color[1] = value.y; color[2] = value.z;
}
```

## `.fzdata` の中身

```toml
type = "PostProcessProfile"

fxaaEnabled = true
exposure    = 1.0

[bloom]
enabled   = true
intensity = 0.8
threshold = 0.7
softKnee  = 0.35

[colorGrading]
enabled    = true
contrast   = 0.0
saturation = 1.0

[[customEffects]]
name       = "Heat Haze"
enabled    = true
shaderPath = "Assets/Shaders/PostProcess/Custom/HeatHaze.hlsl"
intensity  = 1.0
```

## `.fzpp` の扱い — 完全に削除する

**`.fzpp` 形式とその入出力 API を削除し、`.fzdata` (`PostProcessProfile`) へ一本化する。**

**WHY 互換経路を残さないか**: リポジトリ内に `.fzpp` の実ファイルは 1 つも存在しない
(検証済み)。移行対象のデータが無い以上、互換コードは「誰も使っていない第二の経路」に
しかならない。`.fzdata` と `.fzpp` という 2 つのポストプロセスアセット形式が並ぶ状態は、
公開リポジトリとして明確に減点であり、将来の読み手に「どちらを使うのか」という
判断を毎回強いる。

### 削除するファイル

| ファイル | 措置 |
|---------|------|
| `Engine/include/Engine/Asset/PostProcessAsset.hpp` | 削除 |
| `Engine/src/Asset/PostProcessAsset.cpp` | 削除 |

### 修正する参照箇所

| 箇所 | 措置 |
|------|------|
| `AssetBrowserCreate.cpp:10, 246-254` | `.fzpp` 新規作成を `PostProcessProfile` (`.fzdata`) 作成へ置換 |
| `AssetBrowserItems.cpp:259` | `.fzpp` のアイコン定義を削除 |
| `InspectorPanel_Asset.cpp:28, 1434-1480` | `.fzpp` 専用 Inspector 分岐を削除 (`.fzdata` の汎用 DataAsset Inspector が担う) |
| `ScriptProxies.cpp:10, 2867-2872` | `LoadProfile` を `PostProcessProfile` 解決経由へ差し替え |
| `ScriptPostProcessProxy.hpp:52-54` | `LoadProfile` のコメントとシグネチャを更新 |
| `PostProcessInspectorWidgets.hpp:3, 18` | `.fzpp` に言及したコメントを更新 |
| `Assets/Shaders/PostProcess/Custom/CustomPostProcessTemplate.hlsl:10` | `[.fzpp] custom_effects` の記述を `.fzdata` へ更新 |

### `ScriptPostProcessProxy::LoadProfile` の扱い

**API 自体は残し、参照先を `.fzdata` に切り替える。**
削除するのは*ファイル形式*であって、「スクリプトからプロファイルを差し替える」
という*機能*ではない。

```cpp
// 変更前: .fzpp を直接パースして全置換
bool LoadProfile(std::string_view path) const;

// 変更後: DataAssetRegistry が解決した共有実体から全置換
// WHY 共有実体を経由するか: 同じプロファイルを複数箇所から読んでも
//     TOML の再パースが起きず、エディタでの編集が即座に反映される。
bool LoadProfile(std::string_view profilePath) const;
```

## 実装順序

削除を先に行うと、置き換え先が存在しない期間ができる。以下の順で 1 つの変更として進める。

1. リフレクション基盤 (`BeginObject` / 構造体配列) — `PostProcessProfile` の前提
2. `PostProcessProfile` の実装
3. `.fzpp` の削除と全参照箇所の付け替え

**WHY この順か**: 逆順にすると、手順 1〜2 の間ポストプロセスプロファイルを
作成・編集する手段がまったく無い状態になる。ビルドが通らない中間状態も作らない。

## `PostProcessVolumeComponent` の変更

```cpp
struct PostProcessVolumeComponent {
    // 参照するプロファイル。未アサインならインラインの settings を使う。
    // WHY 両立させるか: 既存シーンは settings をインラインで持っている。
    //     参照必須にすると全シーンが壊れる。プロファイルを D&D した時点で
    //     参照モードへ切り替わる、という無改修で移行できる形にする。
    asset::Asset<PostProcessProfile> profile;

    bool  isGlobal        = true;
    int   priority        = 0;      // 小さいものから順に合成する
    float blendWeight     = 1.0f;
    float influenceRadius = 10.0f;  // isGlobal=false 時の球影響半径 [m]
    float blendDistance   = 2.0f;   // 境界の内側でこの距離だけかけてフェードする
    bool  enabled         = true;

    renderer::PostProcessSettings settings;  // profile 未アサイン時のみ使用
};
```

`profile` は `DataAssetRef` を内包し、その `path` が `"Assets/..."` 文字列として
シーンへ保存される。`.fzdata` は既に `AssetDatabase` の拡張子リストにあるため、
`GuidRefCodec` が保存時に自動で `guid:` へ変換する。
**リネーム耐性は追加実装なしで手に入る。**

## ボリュームブレンド

現状 `RenderSystem.cpp:399-403` は最初の `isGlobal` ボリュームを見つけて
まるごと代入しているだけで、`blendWeight` / `influenceRadius` は死んでいる。

```
result = ProjectSettings.postProcess              // ベース
volumes = enabled なボリュームを priority 昇順にソート
for v in volumes:
    w = v.blendWeight * (v.isGlobal ? 1 : DistanceWeight(cameraPos, v))
    if w <= 0: continue
    result = Lerp(result, Resolve(v), w)
```

`DistanceWeight` は `influenceRadius - blendDistance` の内側で 1、
`influenceRadius` で 0 になるよう線形に落とす。

### `LerpPostProcessSettings`

```cpp
// t=0 で a、t=1 で b。
// 数値フィールドは線形補間、bool は t >= 0.5 で b を採用する。
renderer::PostProcessSettings LerpPostProcessSettings(
    const renderer::PostProcessSettings& a,
    const renderer::PostProcessSettings& b, float t);
```

**WHY bool を閾値で切り替えるか**: Unity の Volume framework は
`Overridable<T>` 相当のラッパーで「この項目を上書きするか」をフィールドごとに持つが、
それを導入すると `PostProcessSettings` の 50+ フィールドすべてがラッパー型になり、
シェーダーへの転送・`.fzdata` の形・既存の Project Settings UI が全部変わる。
本設計のスコープを大きく超える。

### セクション単位の override マスク

代わりに **`PostProcessOverrides`** (14 個の bool) をプロファイル側に持たせる。

```cpp
struct PostProcessOverrides {
    bool fxaa = true, exposure = true, bloom = true, /* ... 全 14 セクション */;
    static PostProcessOverrides All();   // 全 true (既定)
    static PostProcessOverrides None();  // 全 false
};
```

**WHY `PostProcessSettings` ではなくブレンド側の型にするか**:
これは「設定値」ではなく「合成時のメタデータ」。`PostProcessSettings` は
シェーダーへの転送元でもあり、Project Settings や runtime override とも共有される。
そこへ override フラグを混ぜると、上書き対象でない経路にも意味のないフラグが付き回る。
独立した型にすることで **`RenderSettings.hpp` は無改修で済む**。

**WHY 既定を全 true にするか**: 全 false を既定にすると、新規プロファイルを作って
値をいじっても画面が一切変わらず「壊れている」ように見える。全 true なら従来の
「まるごと差し替え」と同じ挙動で始まり、不要なセクションのチェックを外していく
引き算の操作になる。インライン設定 (プロファイル未参照) のボリュームも同じ既定に揃う。

これにより **bool の閾値切替の実害も大きく減る**。「ブレンド途中でブルームの
ON/OFF が飛ぶ」問題は、そのセクションを上書きする意図があるときにしか起きない。
上書きしないセクションを素通しできれば、意図しない切り替わりは発生しない。

**残る限界**: 同一セクション内での部分上書き (「ブルームの intensity だけ変えて
threshold はベースのまま」) はできない。ここまで必要になったらフィールド単位の
ラッパー導入を検討する。

`customEffects` (配列) は補間せず、**重みが最大のボリュームのものを採用する**。
**WHY**: 要素数も名前も異なりうる配列を補間する自然な定義が存在しない。

## テスト

`Projects/Tests/Reflection/main.cpp` を新設する。

- [ ] `BeginObject` / `EndObject` が TOML の入れ子テーブルを作る
- [ ] 同名フィールドを持つ別スコープ (`bloom.intensity` / `sharpen.intensity`) が衝突しない
- [ ] `BeginObjectList` が読み込み時に保存された要素数を返す
- [ ] 構造体配列の保存 → 読込で全要素・全フィールドが往復する
- [ ] 要素数 0 の配列が往復する
- [ ] `BeginObject` 未実装のリフレクタでも従来どおり動く (既定実装の後方互換)
- [ ] `PostProcessProfile` の `.fzdata` 往復 (入れ子 12 個 + customEffects 配列)
- [ ] `LerpPostProcessSettings` が t=0 / t=1 で端点を厳密に返す
- [ ] `DistanceWeight` が半径境界で 0、内側で 1 になる

## 実装チェックリスト

### リフレクション基盤
- [x] ✅ `IReflector` に `BeginObject` / `EndObject` / `BeginObjectList` 系を追記 (末尾)
- [x] ✅ `ObjectField` の既定実装を `BeginObject` 経由へ書き換え
- [x] ✅ `kReflectionAbiVersion` を 2 → 3
- [x] ✅ `SceneSerializer` TomlWrite / TomlRead (テーブルスタック化 + 既存 ObjectField 統合)
- [x] ✅ `DataAssetRegistry` TomlWrite / TomlRead
- [x] ✅ `ImGuiReflector` (折りたたみ + 配列 Add/Remove)
- [x] ✅ `ScriptSnapshot` Writer / Reader
- [x] ✅ `JsonReflector` Read / Write / Catalog
- [x] ✅ `FBZZ_OBJECT_LIST_FIELD` マクロ (ユーザースクリプト向け)

### PostProcess
- [x] ✅ `PostProcessProfile` 型 + `Reflect()` 手書き + `FBZZ_REGISTER_DATA_ASSET`
- [x] ✅ `PostProcessVolumeComponent` に `Asset<PostProcessProfile>` / `priority` / `blendDistance` を追加
- [x] ✅ `SceneSerializer` の PostProcessVolume 読み書き更新 (旧形式の読み込み維持)
- [x] ✅ `LerpPostProcessSettings` + `DistanceWeight`
- [x] ✅ `PostProcessOverrides` (セクション単位 override マスク) + プロファイルへの保存
- [x] ✅ プロファイル Inspector の Overrides UI / ボリューム側の「上書きするセクション」表示
- [x] ✅ `RenderSystem` のボリューム合成実装 (priority 昇順 + 距離ウェイト)
- [x] ✅ `InspectorEnvironment` の PostProcessVolume UI (プロファイルスロット + インライン切替表示)
- [x] ✅ AssetBrowser: 新規作成メニューに PostProcessProfile (`.fzdata`) を追加

### `.fzpp` の削除
- [x] ✅ `Engine/include/Engine/Asset/PostProcessAsset.hpp` を削除
- [x] ✅ `Engine/src/Asset/PostProcessAsset.cpp` を削除
- [x] ✅ `AssetBrowserCreate.cpp` の `.fzpp` 新規作成を差し替え
- [x] ✅ `AssetBrowserItems.cpp` の `.fzpp` アイコン定義を削除
- [x] ✅ `InspectorPanel_Asset.cpp` の `.fzpp` 分岐を削除
- [x] ✅ `ScriptPostProcessProxy::LoadProfile` を `.fzdata` 解決へ差し替え
- [x] ✅ `PostProcessInspectorWidgets.hpp` / `CustomPostProcessTemplate.hlsl` のコメント更新
- [x] ✅ `Projects/Tests/Reflection/main.cpp` + CMake 登録
- [ ] Visual Studio 2022 全体ビルド
