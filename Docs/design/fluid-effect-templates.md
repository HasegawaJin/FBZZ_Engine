# 流体素材と複数層の演出テンプレート

既存の FluidRecipe、FluidBakeService、ParticleEmitter と VFX Timeline を使う。新しい再生形式やランタイムのコンポーネントは増やさない。

## 素材プリセット

Fluid Editor の New from preset と Browse templates から作成する。AI の fluid.create も同じプリセットを受け付ける。

| 識別子 | 用途 |
|---|---|
| GroundRing | 着地や地面衝撃の砂煙リング |
| ColdMist | 氷結・冷却・噴出後の低い霧 |
| ChargeVortex | 中央へ集まる発光した渦 |
| EmberBurst | 上へ飛び散って消える火の粉状の流体 |
| SigilFlare | 二重の発光リングが時間差で燃え広がる素材 |

単体プリセットの既定は既存と同じ 3D ベイク。外部 Mask を必要とせず、作成直後からプレビューできる。SigilFlare は幾何学的な輪の見本で、任意の画像マスクによる文字・模様は Texture 発生源へ差し替えて作る。

## 演出テンプレート

Fluid Editor の Effect Templates... → 作成先の新規フォルダ → Create & Bake。

| テンプレート | 素材と開始時刻 |
|---|---|
| Landing | GroundRing 0.00秒 / DustBurst 0.08秒 / ColdMist 0.25秒 |
| Charge Release | ChargeVortex 0.00秒 / PlasmaBurst 0.85秒 / EmberBurst 0.95秒 |
| Magic Eruption | SigilFlare 0.00秒 / PlasmaBurst 0.25秒 / ColdMist 0.50秒 |

操作の登録は fluid.effect_template.create の 1 件。preset は landing / charge_release / magic_eruption、path は Assets 配下の未作成フォルダ。UI と AI はこの操作を呼ぶ。返る jobId は既存の fluid.jobStatus / fluid.cancel のジョブ窓口で追える。

初回の組み合わせ作成は各素材を 128px × 32コマ、2D で順番に焼く。これにより 3D の共有 Baker を占有せず、組み合わせを試す出発点を作る。単独の素材は後から解像度や 3D モードを変更して焼き直せる。

すべての .mat が書けてからフォルダ名と同名の .vfx を作成する。各層はループなし・1粒の burst とし、開始遅延は ParticleEmitter.startDelay、フリップブック再生長は lifetime に置く。VFX Timeline で後から調整できる。参照は AssetDatabase の GUID を使う。

## ファイルと失敗時の扱い

- 各層の .fluid が正本。.mat とアトラスはその隣に出力する。
- 既存フォルダは受け付けない。作成後に編集した .vfx は、素材の通常の Bake では上書きしない。
- 素材のどれかが失敗したら .vfx は作らず、理由をジョブへ残す。作成済みの素材は削除しない。
- キャンセル後も 2D ベイクの書き出し終了までは該当素材を使用中として扱う。
- 一時ファイルへの書き出しと rename に成功してから .vfx の完成とする。

## 検証

FluidEffectTemplateTest で新規レシピの保存・再読込、有限な密度の注入、各層の時間差・burst・素材参照、フォルダの上書き拒否、作成先の検証、キューのキャンセルを確認する。

実機では Create & Bake の完了後に Open Effect VFX で開き、VFX Timeline で開始位置を確認する。素材を通常の Bake で焼き直しても .vfx の調整が残ることを確認する。ビルド・テストは VS Code / Visual Studio のタスクから実行する。
