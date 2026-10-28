<!-- @file    render-extraction-scan-optimization.md -->
<!-- @brief   描画抽出の構造世代・走査融合・比較と安全なフォールバックの契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-24 -->
# 描画抽出の構造世代と走査融合

この変更は通常の描画候補・形状入力を生成するCPU側の最適化であり、BVH方式や可視判定、GPU描画品質を変更しない。現在値の全確認を通知だけで置き換える設計ではない。

## 切替と比較境界

プロセス起動前の環境変数で、同じ実行物の従来経路と候補経路を比較できる。

| 変数 | 値 | 対象 |
|---|---|---|
| `FBZZ_RENDERER_WORKSET_MODE` | `legacy` / `revision` / `validate` | Renderer所属とScene内順序の確認 |
| `FBZZ_RENDER_EXTRACTION_SCAN_MODE` | `legacy` / `fused` | 色可否取得の融合とskinned入力の走査範囲 |

未指定・未知の値はlegacy。workset modeは最初の有効な照会で固定する。scan modeは抽出時に読む。同じSceneに対する並行変更・抽出は従来の契約どおり禁止。既定shadowのlinear、既存BVH/update/producer selector、Script Gizmosや空間診断のON/OFFは変えない。

`RENDERER_WORKSET_CONFIG mode=...` と、scan変数明示時の `RENDER_EXTRACTION_SCAN_CONFIG mode=...` は初回だけ実効選択を出力する。設定文字列だけで各フレームの実経路を証明したとはしない。別のProfiler実行で以下を観察できる。

- `Scene::RendererCandidates`: 所属確認・順序構築・IDコピーを含むCPU scope。
- `Scene::RendererIndexLegacy` / `RendererIndexRevisionReuse` / `RendererIndexRevisionRefresh`: 実際の分岐marker。
- `Scene::RendererIndexRebuild`: 必要になった既存の再整列scope。
- `RenderSceneExtractor::FusedColorEligibility` / `SkinnedRangeInputs`: fused経路marker。

markerは回数の証跡であり、0msの処理時間と解釈しない。親子scopeのinclusive時間は足し合わせない。TimingOnly計測ではProfilerを無効化し、詳細計測とCPUフレームの値を別にする。

## 構造更新の境界

`ComponentArray`はdense Entity列を非公開で所有する。Addは既存の新規lifetimeを構造tokenとして再利用し、Removeと非empty Clearは同じ既存allocatorから新しいtokenを発行する。moveは内容とtokenを移譲し、移動元は空・token0になる。Component値の直接編集では構造tokenは変わらない。

Sceneは所有位置を変えるCompact、MoveToIndex、Fixup、Clearでowner tokenを更新する。末尾への非Renderer生成や削除中のhole化は既存位置を変えず、Rendererの所属変化は配列側tokenで検出する。並べ替え、親子操作、serializer・Undo復元、複製・Play用Sceneの生成も既存の正規構造APIを通る。

`CopyRendererEntities`は要求setに必要な配列tokenとowner tokenを照合する。一致時はID/owner位置の全再比較を省き、既存の順序済みIDをコピーする。不一致時は以前の現在値照合と必要な再整列へ戻る。同数差し替えもtokenで検出する。型ごとの無効化依存を保持し、後から問い合わせたSkinned/Terrain setも更新する。

`validate`はtokenを信用しない別の旧索引へ現在のID/ownerを全照合し、その都度整列して比較する。不一致または未知tokenではready状態を破棄してlegacyから再構築し、そのScene世代ではlegacyに固定する。validateは追加コストを持ち、速度計測用ではない。

保証対象はSceneの正規構造API。公開Iterator内部を使った所有unique_ptrの直接swap/resetやGameObject全体の代入で、ID逆引き・所有位置の不変条件を壊すコードの自動修復は保証しない。従来の全照合でも、そのような不整合の修復を保証していない。今回Iterator APIやGameObject代入の互換性は変更しない。

## 現在値確認と不変snapshot

Transform/Mesh/enabled/layer/Materialなどの公開値の直接変更、親からのworld更新、共有Mesh、skinned poseは従来どおり検証・抽出する。tokenをこれらの完全なdirty通知として使わない。

full抽出ではcallbackがない既存のgeometry append中にMaterialComponent.enabledを読み、同じ全objectを後から再解決するpassを省く。standalone geometryのslot visibility/capabilities抽出は従来の順で残す。材質同期callbackより前に形状・skinned入力を確定する契約は維持する。

static/skinnedはEntity順に混在する。skinnedを単一suffixと仮定せず、最初から最後のskinnedを含む半開区間と、区間内のskinned判定を使う。staticだけなら空区間。cache hitはgeometry所有者pointerと非ゼロsnapshotSerialの一致で範囲を認証し、不明・範囲不正なら全object走査へ戻す。公開済みのgeometry/material値を後から書き換えない。camera、shadow、reflection、Rayの個別状態を共通可視リストへ統合しない。

## 計算量とメモリ

NはScene全個体、Rは要求Renderer候補、Kは構造変更件数、Sは最初から最後のskinnedを含む区間長。Uは型ごとの確保済みsparse領域、Cは確保済みchunk内の構築済みComponent数とする。以下はユーザーcallback自身の任意の計算を除く。

| 操作 | 期待・最悪の境界 |
|---|---|
| 構造変更なしのrevision照合 | 配列数は固定3以下なのでO(1)。出力IDのコピーO(R)は残る。 |
| 構造変更後 | 現在値照合O(R)、必要な並べ替えO(R log R)。K=1でもO(K)更新とはしない。 |
| Add/Remove token更新 | O(1)。Addは既存lifetime発行を再利用、Removeは追加で1回のatomic発行。元の確保・要素move費は別。 |
| Component Clear | 元の実要素破棄O(R)とsparse初期化O(U)を持ち、容量を保持する。token更新だけをO(1)と呼び、操作全体をO(1)とはしない。 |
| 配列move・Scene並べ替え | 内容/tokenの移譲自体はO(1)だが、move先の旧chunk破棄はO(C)を含む。Sceneの所有配列操作・所有位置修復はO(N)。 |
| validate | 全照合と独立再整列を追加しO(R log R)、oracle用O(R)メモリ。 |
| fused色可否 | mandatory traversalへ融合し、追加の全object再解決passを除く。形状生成自体はO(R)。 |
| skinned範囲 | O(S)。staticだけならO(1)、交互配置などでは最悪O(R)。 |

各ComponentArrayとSceneへ構造tokenの保持を追加する。tokenは大小や経過時間ではなく同一性だけを比較し、move後に単調増加する番号とは仮定しない。反復変更は最後のtokenへ集約され、配送queueや購読者を追加しないが、K回の変更時の更新費O(K)は残る。FrameGeometryCacheには範囲認証用32bytesを追加し、同じバイナリ内のlegacy比較でも同じlayoutを使う。sidecarは従来の最大4Sceneを保持し、validate oracleだけ遅延確保する。個別Scene/キャッシュの容量はRに依存し、4Sceneをbyte上限とは扱わない。

公開native layoutが変わるため、C++ SDK consumerは新しいheaderとDLLで再ビルドする。Scene/Materialの保存形式は変更しない。実測、P50/P95/P99、条件別の採否・限界は今回の検証報告を参照する。速度改善やGPUフレーム改善を件数やコンパイル成功だけで主張しない。
