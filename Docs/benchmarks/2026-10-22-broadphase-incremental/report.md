# BroadPhase BVHの差分更新：現行方式との比較

2026-10-22。各substepで中央値BVHを構築する従来方式と、変更葉・祖先をrefitする保持BVHを比較した。

## 条件

- CPU: Intel(R) Core(TM) i7-14700F。Microsoft Windows NT 10.0.26300.0。
- 同じReleaseツリー、MSVC、LTO有効。比較前の実行物とDLLを別フォルダーへ退避し、候補をビルドして交互実行。
- 実行順は旧→新→新→旧→旧→新。CPU affinity mask=16、High priority。GPU・UI・Editorは起動しない。
- 各プロセス・各条件は3反復、それぞれ40呼出しwarmup＋240呼出し計測。720生標本/プロセス、各方式3プロセス。
- 代表値は3プロセスの中央値の中央値。p95列も各プロセスp95の中央値。p95はnearest-rank。単位はµs/呼出し。
- BroadPhase単体では入力の移動・結果検証を計時から除外する。WorldはBegin/EndSceneSyncとStepを含む。

## 結果

| 条件 | 旧 median | 新 median | 変化 | 旧 p95 | 新 p95 |
|---|---:|---:|---:|---:|---:|
| broadphase/256/unchanged | 14.900 | 3.300 | -77.9% | 19.200 | 3.400 |
| broadphase/256/one | 14.900 | 3.500 | -76.5% | 15.900 | 3.600 |
| broadphase/256/one-percent | 14.900 | 3.500 | -76.5% | 15.100 | 3.600 |
| broadphase/256/all | 14.900 | 5.900 | -60.4% | 17.300 | 5.900 |
| broadphase/256/shuffle | 18.300 | 18.600 | +1.6% | 19.100 | 20.300 |
| broadphase/256/churn | 19.600 | 19.700 | +0.5% | 20.200 | 20.900 |
| broadphase/256/static | 14.600 | 2.800 | -80.8% | 15.000 | 2.900 |
| broadphase/256/sleep | 14.800 | 3.000 | -79.7% | 16.400 | 3.100 |
| broadphase/256/cold | 15.200 | 17.100 | +12.5% | 17.500 | 27.900 |
| broadphase/5000/unchanged | 1017.700 | 247.600 | -75.7% | 1271.800 | 324.700 |
| broadphase/5000/one | 1018.600 | 249.000 | -75.6% | 1286.800 | 315.200 |
| broadphase/5000/one-percent | 1015.000 | 250.400 | -75.3% | 1276.800 | 351.000 |
| broadphase/5000/all | 1029.400 | 377.000 | -63.4% | 1306.500 | 515.500 |
| broadphase/5000/shuffle | 1038.700 | 1059.900 | +2.0% | 1288.600 | 1332.500 |
| broadphase/5000/churn | 1011.800 | 1037.400 | +2.5% | 1275.500 | 1262.300 |
| broadphase/5000/static | 916.600 | 158.000 | -82.8% | 1147.400 | 196.900 |
| broadphase/5000/sleep | 919.800 | 161.600 | -82.4% | 1174.600 | 205.700 |
| broadphase/5000/cold | 1073.600 | 1057.700 | -1.5% | 1390.800 | 1310.900 |
| world-sync-step/256/free-motion | 24.500 | 14.900 | -39.2% | 31.300 | 16.100 |
| world-sync-step/1024/free-motion | 143.900 | 60.300 | -58.1% | 199.800 | 77.600 |

## 実装と適用範囲

- 現在の全AABBを照合し、変更された葉と祖先だけを更新する。ColliderやBodyのpointerは保持しない。
- 所属変更時、旧葉から離れた移動がmax(8, proxy数/8)を超えた場合、正規化した内部表面積コストが構築時の1.5倍を超えた場合は再構築する。
- static/sleep/trigger/layerとbody対応は現在値で判定する。Worldの同期・sleep early-out条件は変更しない。
- unchanged/one/one-percent/allは、互いに離れた2個ずつの群を使う。allは全体が同じ方向へ動くcoherent motionである。
- shuffleは群の位置を大きく入れ替える。churnは末尾Colliderのnull/有効を交互に切り替え、所属再構築を発生させる。
- static/sleepは全個体がその状態。triggerはこの性能入力に含めず、自動回帰で検証する。
- coldは各呼出し前にsolverを作り直す。旧方式のthread-local容量は残る一方、新方式ではsolver所有領域が初期化されるため、その寿命管理費用も含む。
- 全件AABB照合と候補探索は残る。追加・削除の局所挿入、静的/動的別の木、接触pair cacheは今回の対象外。

## 正しさと限界

- すべての計測標本で候補件数・重複・対応する群を検証した。Worldの測定は独立した剛体の移動と有限性を確認する。
- PhysicsAutoは通常Release・LTO有効Releaseで各596件合格。葉refitを意図的に無効化した場合に追加テストが失敗することも確認した。
- Engine、Editor、SandboxStandaloneもRelease/LTO有効でビルドした。測定したDLLと最終出力の一致、LTO設定の復帰、検証ログはvalidation.jsonに記録した。
- 木を保持するため移動後の候補列挙順は従来方式と異なり得る。PGS結果のbit単位一致は保証しない。
- 標準allocatorの確保回数、常駐メモリ量、GPU時間、Editor/ゲーム全体のFPSは計測していない。
- 速度差はこの固定入力の結果であり、接触密度の高い任意のゲームシーンへ同じ改善率を保証しない。
- プロセス間の中央値の範囲はsummary.json、実行物とソースのSHA-256はraw/environment.json、生標本はraw/以下に保存した。

再実行入口: `FBZZPhysicsBroadPhaseBench.exe --json <出力> --warmup 40 --steps 240 --repeats 3`。ビルドは `Tools/AgentBuild.ps1 build FBZZPhysicsBroadPhaseBench -Preset release`。

設計: [BroadPhaseの差分更新](../../design/broadphase-incremental.md)。
