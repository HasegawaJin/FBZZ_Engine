# ゲーム内設定 — スクリプトから 1 行で増やす

> 関連: [開発計画](development-plan.md) ／ エンジン側の設計は `Docs/design/game-settings.md`
> 実装: `Scripts/Game/GameSettingsRegistry.hpp` `Scripts/Game/GameSettingsComponent.hpp`
> `Scripts/Title/OptionsScreenComponent.hpp`

設定は **宣言簿 (registry) が正本**。スクリプトが 1 つ宣言すれば、保存も Option の行も
読み口も同時に手に入る。

---

## 増やし方

### 1. 宣言する（どのスクリプトの `OnStart` でもよい）

```cpp
#include <Scripts/Game/GameSettingsRegistry.hpp>

void OnStart() override
{
    settings::DeclareToggle("bloodEffects", "Tab_GAME", true);
    settings::DeclarePercent("uiOpacity",   "Tab_GAME", 0.9f);
    settings::DeclareNumber ("aimAssist",   "Tab_GAME", 0.45f, 0.0f, 1.0f, 2);
    settings::DeclareChoice ("difficulty",  "Tab_GAME", 1, { "易しい", "普通", "難しい" });
}
```

同じ id を毎回宣言してよい。**宣言は「どんな項目か」で、値は上書きしない** ―
シーンを移るたびに設定が既定へ戻ることはない。

### 2. 読む

```cpp
if (settings::GetBool("bloodEffects")) SpawnBlood();
const float assist = settings::Get("aimAssist", 0.45f);
const int   level  = settings::GetInt("difficulty");
```

`GameSettingsComponent` が居ないシーンでも最後に効いていた値が返る。
未宣言の id は第 2 引数（fallback）を返すので、宣言の順番を気にしなくてよい。

### 3. Option 画面に出す（任意）

シーンの `Tab_GAME` の下に `GAME_Row_<id>` という名前の行オブジェクトを置く。
既存の行を複製して名前を変えるだけでよい（子の `Label` / `Value` / `Slider` /
`Track` / `Fill` / `Knob` は名前が固定）。

⚠ **行は 4 か所にある。** `Options.scene` と、`Stage_01`〜`Stage_03` のポーズ
（`Pause_Canvas > Pause_Root > Pause_Options`、[game-flow.md](game-flow.md#ポーズstage_01stage_03)）。
ポーズ側には CONTROLS（キーコンフィグ）を置いていないので、行を足すときに触るのは
タブ 4 枚ぶんだけでよい。**片方だけに足すと「タイトルからは直せるのに、遊びながらは
出てこない」**という形で欠ける。

**置いた行だけが出る。** 宣言の `page` は覚え書きで、実際にどのページへ出るかは
オブジェクトを置いた場所が決める。同じ id を 2 ページへ置くこともできる
（INPUT の `device` がそうなっている）。

行を置かなければ画面には出ないが、保存と読み口は動く ― 隠し設定になる。

---

## 保存

| テーブル | 中身 | 正本 |
|---|---|---|
| `[video]` `[audio]` `[input]` `[game]` | 組み込み 24 項目 | `GameSettingsComponent` の構造体 |
| `[bind]` | キーコンフィグの差分 | 同上 |
| `[settings]` | **スクリプトが宣言した項目** | 宣言簿 |

保存先は `%LOCALAPPDATA%/GreenWare/settings.toml`（`Per User` を切ると実行ファイルの隣）。

> **WHY 組み込みを `[settings]` へ移さないか:** 表示・音量・入力の値は
> 「効かせる手順」と対で意味を持つ（解像度は先に決めてからモードを切り替える、
> 画質は未選択なら触らない）。値だけ宣言簿へ移すと、その手順がどこにも属さなくなる。
> 宣言簿へは **出し入れの口だけ**を差してある。

---

## 効かせる

値が変わった瞬間に何かしたいなら `onApply` を渡す。

```cpp
settings::DeclarePercent("uiOpacity", "Tab_GAME", 0.9f,
                         [](float v) { if (auto* hud = HudComponent::Instance()) hud->SetOpacity(v); });
```

渡さなければ「保存される値」になるだけで、読む側が好きなときに引く。

---

## 落とし穴

- **`Setting*` を控えない。** 宣言が増えると宣言簿の配列が再確保され、
  控えたポインタは無効になる。`settings::Get(id)` をその場で呼ぶこと。
- **`Set` は宣言簿を触る。** ループ中に `Set` を呼ぶなら、そのあと id で引き直す。
- **DLL をリロードすると宣言は消える。** 宣言する側が `OnStart` で毎回宣言するので、
  次のフレームには揃い直す。値は config から読み戻される。
- **`ResetToDefaults` は宣言簿が値を持つ項目だけ戻す。** 組み込みは
  `VideoConfig{}` のような構造体の初期化子が既定なので、あちらが自分で戻す。

## 廃止済み設定（2026-09-15）

チェイン表示の有効・無効設定は廃止。Optionsと戦闘中のポーズ設定から行を削除し、保存対象にも含めない。連撃が成立したときのHUDは常に表示する。
