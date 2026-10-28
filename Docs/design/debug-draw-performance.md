<!-- @file    debug-draw-performance.md -->
<!-- @brief   多数ライトの範囲表示で円周の不変計算を共有する。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-10 -->

# デバッグ円の計算再利用

NightLightsLabの診断では、Scene ViewのLightRangeDebugが遅いフレームで約5～6 msを使った。DebugDrawの球は三つの円で表され、各円は24線分。従来は線分ごとに両端のsin/cosを求め、同じ円周係数と隣接頂点を毎ライト・毎フレームで再計算していた。

円周係数25点をGraphics実装内の不変配列で一回だけ求める。角度は既存のstep * index、頂点はcenter + axis1 * (radius * cos) + axis2 * (radius * sin)の順を保つ。最終端点も既存の2πの係数を使い、始点へ強制的に丸めない。隣接線分の共通端点は求めた値を使い回す。任意角のArcとCapsuleの半円は既存の処理を維持する。

24線分、色、半径、向き、頂点順、全ライトの表示、バッチの上限・flush・所有権を維持する。配列は初回初期化後に書き換えず、変換済みの座標やGPU資源は保持しない。公開APIとRHIの契約を追加しない。[MSVCのlocal static初期化](https://learn.microsoft.com/en-us/cpp/build/reference/zc-threadsafeinit-thread-safe-local-static-initialization?view=msvc-170)の完了後は読み取りだけにする。

既存のDebugDrawCaptureで球の三閉路・頂点数・色・半径、任意法線の円が属する平面、別中心と半径を続けて描いた場合の出力を確認する。測定は共有Engine/Shaderの更新後にbaselineを取り、同じDLL群・NightLightsLabの1,000灯・sampled-8・CPU geometry・Scene/Game 1904×893で、このGraphics変更だけを比較する。

診断snapshotの取得はフレームへ追加の負荷を与えるため、同区間の値を600フレームの無問い合わせ性能窓と比較しない。GPU timestampにもframe ageがあり、同時刻のCPUスパイク原因をGPUの単一値で断定しない。測定結果は[VolumetricLight](volumetric-light.md)へ記録する。

## 2026-10-10 の検証

共有コード更新とTerrainの端パッチ修正を取り込んだ後、同じEngine/Core/Shaderソースを固定し、旧円生成へ一時復帰して600フレームを採取した。再び最終実装へ戻して2回の600フレームを採取した。DLLは依存関係の再リンクでhashが変わるため、同一DLLバイト列の比較とは記載しない。

| 方式 | frame中央値 | p95 | p99 | 最大 | LightRangeDebug CPU中央値 |
|---|---:|---:|---:|---:|---:|
| 従来の円生成 | 13.710 ms | 16.447 ms | 20.499 ms | 26.557 ms | 2.563 ms |
| 円周係数・隣接端点の再利用 | 12.635 ms | 13.970 ms | 15.001 ms | 16.604 ms | 1.618 ms |
| 再利用の再確認 | 12.776 ms | 16.179 ms | 19.894 ms | 39.572 ms | 1.471 ms |

frame中央値は約7～8%、対象CPU scopeは約37～43%低下した。CPU scopeは性能窓後の16個の詳細snapshotで、600個すべてのpass時間ではない。最大frameの内訳とも対応しない。最初の再利用測定は16.67 ms超が0件、再確認は29件だったため、スパイク解消は主張しない。中央値換算は約78～79 FPSで、Editor全体120 FPSは未達。

VolumetricLightのGame GPU中央値は1.616 / 1.620 / 1.614 ms。積分・散乱・解像度・step・light budgetは変更していない。24分割の球と非軸方向の円、異なる中心・半径・色の連続呼び出し、Arc/Capsule分割数の3件を含め、同期と水中passのCPU35件が合格した。最終EditorLauncher/GraphicsAutoのReleaseビルドと規約検査はエラー・警告0件。

記録: `build/agent/build-20261010-193556-41504.log`、`build/agent/test-20261010-193729-51324.log`。性能とScene/Game画像は `Scratch/VolumetricLightFixCheck-01a120b5/DebugCircleMatchedBaseline`、`DebugCircleMatchedAfter`、`DebugCircleMatchedAfterRepeat` の各 `NightVolumetricOn600.report.json`。SDKへの反映は保留。

同じ共有Graphicsコードを含む別チャットの最終DX12回帰も172件すべて合格した。XML上は全173件・disabled 1件・failures/errors 0、実行時間93.356秒（`Scratch/ProjectSimplifierGpu.xml`）。初回の共有fixture失敗は検証設定・アセット検索先・サービス後始末の修正後に解消された。
