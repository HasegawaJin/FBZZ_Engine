# Cursor — 要求スタックと見た目の分離

カーソルの「拘束・表示」をスクリプトの要求スタックへ移し、プロジェクト設定には
「絵」だけを残す。あわせて Editor の Game View に状態オーバーレイを置く。

対象: [Cursor.hpp](../../Projects/Engine/include/Engine/Core/Cursor.hpp) /
[ScriptCursorProxy.hpp](../../Projects/Engine/include/Engine/Scene/ScriptProxy/ScriptCursorProxy.hpp) /
[ProjectSettings.hpp](../../Projects/Engine/include/Engine/ProjectSettings.hpp) /
[ViewportPanel.cpp](../../Projects/Editor/src/Panels/ViewportPanel.cpp)

---

## 1. 何が壊れていたか (2026-09-06 以前)

| 症状 | 原因 |
|---|---|
| Escape で解放した後、ゲーム画面をクリックしても捕獲へ戻らない | 解放フラグを畳むのが「Game View からフォーカスが外れたとき」だけだった |
| Play 中に Project Settings を触るとスクリプトの要求が消える | `[cursor]` が «起動時の初期値» と «実行中の要求» で同じ 1 変数を共有していた |
| 画面内に主張者が 2 つ居ると順序で結果が変わる | 要求が単一のグローバル変数で、最後に呼んだ側が勝つだけだった |
| 遷移先に主張者が居ないと前の画面の拘束が残る | シーン切り替えで畳む経路が無かった |

3 番目と 4 番目は当時のシーン構成 (Stage 系と メニュー系が別シーン) で表に出ていなかった。
ポーズメニューを Stage の中に置いた瞬間に踏む。

---

## 2. 3 層の状態

```
  要求スタック  ← ゲームが積む (cursor.Push)。優先度つき。同値なら後勝ち
       ↓ 勝者
      基底      ← cursor.SetLockMode / SetVisible。誰も Push していないときの値
       ↓
     実効値     ← IsVisible / GetLockMode / GetEffectivePolicy が返すもの
       ↓ ここから先は «OS へ流すか» の話
     抑制       ← Cursor::SetSuppressed (Editor 専用)・ウィンドウ非アクティブ
```

**要求と抑制は別物**。抑制はゲームの要求を消さずに OS への反映だけを止める。
消してしまうと「戻ってきたときに何を復元するか」が分からなくなる。

### 優先度

| 定数 | 値 | 用途 |
|---|---|---|
| `CursorPriority::Camera` | 0 | 視点操作。ゲームの地の状態 |
| `CursorPriority::Gameplay` | 100 | 照準・ドラッグなど遊びの都合 |
| `CursorPriority::UI` | 200 | ゲーム内カーソル・メニュー |
| `CursorPriority::Modal` | 300 | ポーズ・ダイアログ |

---

## 3. 書き方

```cpp
class PauseMenu : public Script {
    FBZZ_SCRIPT(PauseMenu)
    CursorRequest m_cursor;      // ← メンバーで持つ。これが要ぶん

    void Open()  { m_cursor = cursor.Push(CursorLockMode::Confined, true,
                                          CursorPriority::Modal); }
    void Close() { m_cursor.Release(); }   // TPS カメラの Locked へ自動で戻る
};
```

- **戻す値を覚える必要が無い**。`Release` すると、残っている要求のうち最も強いものが実効値になる。
- **取り下げ忘れが起きない**。`CursorRequest` のデストラクタが `Release` するので、
  スクリプトの破棄・シーン遷移・DLL リロードのどれでも自動的に外れる。
- 主張し続けたまま中身だけ変えるときは `Set(mode, visible)`。`Release` → `Push` だと
  同値の後勝ち順序が変わり、他の要求との強弱が黙って入れ替わる。
- 主張者が 1 つしか居ない画面なら `SetLockMode` / `SetVisible` (基底) で足りる。

### Confined と Locked

`Locked` は毎フレーム OS カーソルを中央へ戻し、移動量だけを渡す。**絶対座標が消える**ので、
画面座標を読んで自前のポインターを描くゲームは中央に貼り付く。「画面から出さない」だけが
目的なら `Confined`。非表示だけではマルチモニターで破綻する (`ShowCursor` はスレッド単位で、
他アプリのウィンドウ上ではそのアプリのカーソルが出る) ため、拘束と対で使うこと。

### `FBZZ_EXECUTE_ALWAYS` の注意

編集中も `OnStart` / `OnUpdate` が走るので、`app.IsPlaying()` で囲まないと
**シーンを開いただけで Editor のカーソルが消える**。Editor 側も非 Play 中は
毎フレーム `SetSuppressed(true)` を張る防波堤を持つが、要求自体は積まないこと。

---

## 4. シーン遷移

`SceneManager::Update` の切り替え地点で `Cursor::ClearRequests()` を呼び、
**要求も基底も既定へ戻す**。スクリプトの要求は Scene の破棄で自動的に外れるが、
`SetLockMode` で基底を直接書いた分は誰も外さないまま次の画面へ持ち越される。
新しい Scene のスクリプトは同じ Update で名乗り直すので、既定へ戻る瞬間は画面に出ない。

---

## 5. 見た目 (ProjectSettings `[cursor]`)

設定が持つのは差し替え可能な «絵» だけ。

```toml
[cursor]
hardware = true

    [cursor.shapes.Default]
    image = 'Assets/Sprites/HUD/pointer_c_shaded.png'
    hotspot = [ 2.0, 2.0 ]
```

種類は `Default` / `Clickable` / `Grab` / `Text` / `Aim` / `Busy`。ゲームは
`cursor.SetShape(CursorShape::Clickable)` のように **種類だけ** を指す。
どの絵を当てるかはプロジェクトの見た目の話で、素材を差し替えるたびに
スクリプトを直すことにならない。

読み込みは Standalone の起動時と Editor の Play 開始時の 1 回だけ。差し替え先は
ウィンドウクラスのカーソルで、Editor では «エディタの窓全体» が対象になるため、
編集中には適用しない (Play を抜けると矢印へ戻す)。

### ハードウェアとソフトの使い分け

自前で UIImage を動かすカーソル ([GameCursorComponent](../../GreenWare/Assets/Scripts/Utils/GameCursorComponent.hpp))
は «前フレームの位置» を描くので、マウスを速く振ると必ず遅れて見える。
マウス操作中は OS のカーソル (`visible = true` + `Default` の画像)、パッド操作中は
UIImage、と機器で切り替えるのが正解。パッドは OS カーソルを動かせないので選択肢が無い。

---

## 6. Editor

- **Game View 左下のオーバーレイ** — 実効モードを常時表示。クリックで要求スタック
  (優先度・要求者・内容) と `Cursor: Game / Free` を開く。
- **解放中は枠 + 「Click to capture」** — 画面内をクリックすれば捕獲へ戻る。
- **Escape** — カーソルを取り上げているとき 1 回目は解放、2 回目で Stop。
  自由なカーソルなら Escape はゲームのもの (Play Focused だけ従来どおり Stop)。
  `ImGui::GetIO().WantTextInput` 中は横取りしない。
- 拘束範囲は Game View の矩形 (デスクトップ座標)。パネルを Dock から剥がしても追従する。
