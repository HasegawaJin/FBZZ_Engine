# GreenWare と Engine の機能境界

2026-09-13。依存方向は `GreenWare → Engine → Physics → Math`。
ラグドールの境界は [ragdoll-api.md](ragdoll-api.md)、音声の所有権は
[audio-system.md](audio-system.md) に従う。

## 今回集約した機能

| 元の実装 | 汎用機能の正本 | GreenWare に残す判断 |
|---|---|---|
| ScreenProjection、GameCursor の三角関数による投影 | ScriptCameraProxy の TryWorldToViewportPoint / TryViewportToWorldPoint | エフェクトの中心、カーソルから狙う深さ |
| WeaponSockets と4スクリプトの再帰検索 | GameObject::FindInSubtree | 装着点・部位の名前、旧名へのフォールバック、検索結果のキャッシュ |
| BodyBounds の寸法・スケール計算 | ScriptColliderProxy::TryGetPrimitiveWorldBounds → ColliderSync → Physics 形状の GetAABB | UI の既定高さ、ビーム判定用の近似半径 |
| SeWarmup の音量0による再生 | ScriptAudioProxy::Preload → AudioManager::AcquireClip | 対象の音群、ロードする順番と1フレームあたりの本数 |
| GameCursor の Canvas Scaler 数式 | ScriptUIProxy::TryGetCanvasSize → UISystem の既存 Canvas 寸法計算 | 使用する Canvas、入力機器の切り替えとカーソル位置 |
| BeamGeometry、LaserVolley、PlayerBossBlock の点と線分の距離計算 | Math/Segment.hpp | ビームの太さ、接触時のダメージ、押し戻し方 |

既存のゲームヘルパーは呼び出し形を保つ転送とゲーム上の判断のみを持つ。
使用されなくなった ScreenProjection.hpp と随伴 .meta は参照を検索したうえで削除した。
公開する数値は Math 型を使い、physics::Collider や renderer::Camera の実体をゲームへ渡さない。

## API の契約

### Transform

Script の `transform.position` は親に対するローカル座標、`transform.worldPosition` はワールド座標。
どちらの書き込みも、その場で自分と子孫の world 値を更新する。
`worldPosition` は祖先のローカル姿勢から親変換を求め、対応するローカル位置へ逆変換するため、
次の PrePhysics でも指定した位置を維持する。同じワールド座標を `position` へ重ねて書かない。
親のゼロスケール軸では逆変換できない成分をローカル 0 とし、実現できる world 値を返す。
位置の変更でローカル回転・スケールは変えない。読み取りは描画用に公開された姿勢を返す。

この同期は ScriptTransformProxy の契約。`GameObject*` から触る `object->transform` は
Engine の Transform データなので、world 値への直接代入はローカル値を更新しない。
ルートの生成物を直接配置し、同フレームに描画・物理の初期値として読む処理では、
既存のローカル・world 両方の設定を維持する。親付きの直接配置では親変換を考慮する。

### カメラ

`TryWorldToViewportPoint(world, outViewport, cameraObject)` は左上原点の UV と DirectX 深度を返す。
UV が 0..1 の外でも成功する。背後・投影不能・カメラ不在では false、出力は変更しない。
`IsVisible` は UV と near/far の深度範囲の両方を見る。遮蔽物による可視性は判定しない。

`TryViewportToWorldPoint(uv, depth, outWorld, cameraObject)` の depth はカメラ前方への距離 [m]。
既存 `ScreenToWorldPoint` の z (DirectX 深度) とは別の単位である。
対象省略時は自分の CameraComponent、無ければシーンの有効なメインカメラを使う。
明示した無効カメラから別のカメラへは黙って切り替えない。
既存の pixel 座標APIも同じカメラ選択へ統一した。

### 階層検索

自分を含み、子の表示順の深さ優先で最初の一致を返す。非アクティブも検索する。
別の部分木へは出ない。探索スタックはヒープ上の一時配列とし、深い階層でC++の再帰を使わない。
戻り値は非所有の GameObject ポインターで、シーン変更をまたぐ保持には EntityID を使う。

### 境界

球・カプセル・箱・AABB・円柱に対応。同一 GameObject に複数あれば統合する。
無効な形状も含むオーサリング設定の照会であり、現在の衝突対象だけを返すAPIではない。
Transform の world 値が更新済みであることを前提にする。

寸法は物理同期と同じ関数で作り、中心には符号付きスケールと回転を適用する。
照会用の形状はスタック上で作り、登録済み物理形状・ハンドル・接触状態を変更しない。
メッシュ・凸包・地形だけの対象は未対応として false を返す。

ゲーム側の BodyBounds は回転後の AABB から top/bottom を求める。
CenterWorld は x/z の中心オフセットも反映する。半径は水平半幅の最大値による近似で、
厳密な衝突判定ではない。この変更で回転した箱や横倒しのカプセルの表示位置・判定幅は変わる。

### 音声と UI

`audio.Preload(path)` は同期処理で、AudioSource を必要とせず、再生要求・voice を生成しない。
PCM は既存のパスキャッシュに入り AudioManager 終了まで保持する。失敗は false とログで通知する。
SeWarmup は失敗数を Inspector に出し、読み込み対象の選択と分割処理を引き続き担当する。

`ui.TryGetCanvasSize(outSize, canvasObject)` は実際のゲームビューポートを使う。
safeArea を差し引く前の Canvas 全体の寸法で、Overlay のスケールと WorldSpace の固定寸法を
描画・レイアウト側と同じ関数で解決する。カメラのアスペクト比を書き写さない。

### 線分

Math/Segment.hpp は最近接点と距離のみを扱う。長さ EPSILON 以下は始点へ縮退する。
長さの二乗と EPSILON を直接比較していた旧実装を修正し、短い線分も適切に投影する。
ダメージ対象の選別・極性・太さの合成は Math や Physics へ移さない。

## ゲーム側に残すもの

LaserVolley の地面への着弾判定は、登録済みの具体的なコライダー型を取得し、
`RaycastHit::collider` と所有する形状が一致したものの `isTrigger` を確認する。
基底 `ColliderComponent` は ECS の登録型ではないため `GetComponent` の型引数にしない。
同一物体に実体とトリガーが併存しても、実際に当たった形状だけで除外を判断する。

RagdollPresentation の常時ふらつき、ボス脚の除外名、衝撃から角速度を作る演出はゲーム方針。
WeaponSockets のクリップ名と持ち替え時刻、BeamTrailRenderer のビーム固有の波と配色、
VfxManager の効果選択と同時発生数の制御もゲーム側に残す。
GPU転送・物理拘束・音声デコードなど、システムの実行処理とは分離する。

## 検証

ScriptSceneQueryTests を EngineAuto、SegmentTests を MathAuto に登録した。
投影の往復、失敗時の出力保持、画面外/背後/near/far、明示カメラ、階層検索順、
回転/負スケール/複数形状、物理形状の非変更、Canvas Scaler と有限線分の境界を対象とする。

差分の静的チェックを実施。C++ビルド、テスト実行、見た目・音声の実機確認は未実施。
AGENTS.md の指定に従い VSCode / Visual Studio で全体ビルドし、Editorを再起動して Scripts を
再コンパイルする。MathAuto / EngineAuto の実行と、Stage_01 / Stage_02 での確認が必要。
