# Cloth — シミュレーションと布の反射

作成日: 2026-09-17。段階的に導入する。ユーザー承認済みの責務分割を正本とする。

## 両面と Wireframe の表示 (2026-09-20)

両面描画の法線は、法線マップ適用前の法線と視線で表裏を決めてから、評価済みの法線を視点側へ向ける。負スケールで三角形の巻き順が反転しても照明半球が逆転しない。IBL がないときは共通 `ambientColor` を使い、直接光は他の材質と同じ `LIGHT_UNIT_SCALE` で評価する。Wireframe へ切り替える際も材質の Double Sided・Blend・Depth Bias を保持する。

検証: 関連 171 テスト合格 (`build/agent/test-20260920-031417-36332.log`)。変更 C++ 6 ファイルと Engine テスト用ビルドは警告・エラー 0。Cloth / Water の両コピーの VS / PS は DXC SM 6.8 でコンパイル成功。表裏の目視比較用 `ClothDoubleSided.playtest.json` と水面の表示切り替え用 `WaterWireframe.playtest.json` を追加し、JSON 構文を検証した。シナリオの実行・実画面の確認は未実施。

## FlowField の受信 (2026-09-20)

`receiveFlowFields` は既定 false、`flowFieldChannels` は全 32 bit。受信時は Engine の共通 `FlowReceiver` / `MakeFlowSampler` を使い、Scene の不変な FlowFrame と時刻を固定する。Physics には位置から媒質速度 [m/s] を返す関数だけを渡し、各 substep の現在の三角形重心で評価する。環境風・局所場の合計を既存 `windVelocity` に加え、面法線方向の相対流速と面積から既存の抵抗を計算する。固定点は移動しない。非有限の標本値は Step 全体を巻き戻す。

Water / Particle / Fur と受信判定・場の評価を共有する。力や描画への変換は各消費者が持つ。Cloth は場の外でも既存の静止空気に対する抵抗を保ち、Particle は covered=false なら緩和しない。詳細は [FlowField](flow-field.md)。

検証: Cloth / Flow / Water / Fiber / Particle 関連 257 件合格 (`build/agent/test-20260920-024029-14896.log`)。新規テストで三角形重心・substep の標本化、一様風との加算、ピン、非有限値の巻き戻し、局所場・渦・環境風・受信設定・保存往復、共通受信の流速一致と snapshot の寿命を確認。変更 C++ のコンパイル、Engine / Editor テスト用ビルド、AgentLint、両シェーダーコピーの Particle CS / Fiber Shell VS の DXC SM 6.8 コンパイルは成功。ビルドには今回の変更箇所以外で警告 7 件がある。ClothSmoke の受信 ON 手順と JSON 構文を確認したが、実画面・シナリオ実行は未実施。

## 別 Cloth 間の面・辺接触と CCD (2026-09-20)

- 双方の `interCollisionDistance` を正値にし、どちらかの `interCollisionFaces=true` で質点–三角形・辺–辺の近接を追加する。`interContinuousCollision=true` は面・辺接触も有効化し、固定更新全体の開始位置から終了位置までを掃引する。自己衝突の `selfCollisionFaces` / `continuousCollision` とは独立。既定は両方 false。
- 有効な組は相互の collisionMask と World のレイヤー行列に従う。最近点の重心座標・辺の媒介変数を使い、4 質点へ逆質量比で補正を分配する。境界・端点と、潰れた三角形の辺も含む。異なる布の静止距離による除外は行わない。固定点・完全追従点は動かさず、最大移動距離が最後に優先される。
- CCD は両 primitive の最大相対変位を距離減少の上限とする conservative advancement。厚みに達するまで安全な時間増分を積むため、同一平面内の移動も対象となる。1 組 128 回、停止許容差は max(1 μm, 接触距離の 0.01%)。接触時刻の重み・法線に対して終了位置を射影する。128 回で判定できない入力は接触なしとせず、相互衝突全体を失敗させる。
- 各反復で swept AABB の候補を取り直し、計 4 反復で補正する。広域探索は各反復 200 万走査まで、CCD 距離評価は相互衝突の呼び出し全体で 200 万回まで。上限・非有限値では全参加者の位置・速度を呼び出し前へ戻す。個別 Step と既に剛体へ返した力積は取り消さない。
- 固定更新の開始/終了間の直線軌跡を仮定し、substep ごとの曲がった軌跡は保存しない。初期交差の解消や有限反復での完全な非交差保証、相互摩擦は含まない。矛盾する移動拘束や、最後の相互補正による外部 Collider への侵入の優先順位は従来どおり。
- リファレンス: [Bridson et al. 2002 §6.2](https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf)、[Codimensional IPC §5.3–5.4](https://ipc-sim.github.io/C-IPC/file/paper.pdf)、[Ericson Closest Points](https://realtimecollisiondetection.net/)。距離下界と接触要素を参考にしており、IPC のエネルギー最適化や厳密な非交差保証を実装したものではない。実装の直近にも `@see` を記載する。

検証: Cloth 関連 86 件すべて合格 (`build/agent/test-20260920-010011-26968.log`)。今回追加した Physics 11 件と Engine 1 件で、質点球の隙間に対する面・辺近接、高速貫通の CCD 有無の対照、固定更新全体の掃引、動く固定面、マスクと距離 0 の除外、自由質点間の運動量、移動制約優先、再現性、同一平面内の侵入、ほぼ平行な辺、探索上限時の全参加者の巻き戻し、シーンからの設定伝播を確認した。保存往復には両設定を追加。変更 C++ の個別コンパイルと AgentLint、Physics / Engine / Editor のテスト用ビルドも成功。ClothSmoke に面・辺接触と CCD の有効化を追加し JSON 構文を検証したが、シナリオ実行・実画面確認は未実施。

## 環境 sheen・二面角・接触の拡張 (2026-09-20)

- Cloth の環境 sheen は Charlie / Neubelt BRDF の 64 点一様半球求積。既存環境キューブの未畳み込み mip 0 を読み、GGX の粗さ mip・DFG LUT は流用しない。sheen 色、roughness、IBL intensity / specular scale、AO を反映し、sheen=0 では追加サンプルを省く。各画素に最大 64 回の環境サンプルが増える。実機 GPU 時間は未測定で、鋭い HDR 光源では有限サンプルによる誤差がある。
- `settings.dihedralBending=true` で隣接三角形の符号付き二面角を XPBD で拘束する。初期角度からの最短角度差と解析勾配を使う。`bendCompliance` の単位はこのモードでは 1/(N m)、旧距離近似では m/N。潰れた面はその反復で解かない。互換性のため既定 false。
- `collisionMask` は相手の GameObject layer に対する 32 bit マスク。Inspector / 保存では符号付き整数、-1 が全層。World の LayerCollisionMatrix と両方を満たす有効な非 Trigger Sphere / Capsule を対象とする。手動の groundHeight 平面はレイヤー対象外。
- 動的剛体は並進・角速度を読み、接触点の `v + ω × r` との相対速度で摩擦を計算する。それ以外の形状は成功した固定更新間の Transform 差分を使い、初回と teleportDistance 超の移動は速度 0。`attachToParentBody` は最寄りの有効な祖先剛体を使う。摩擦は従来どおり接線速度の減衰率で、Coulomb 摩擦ではない。形状 CCD は並進だけを扱い、回転掃引は未対応。
- `twoWayCoupling=true` では正質量の動的剛体の逆質量・逆慣性を接触の有効質量に含め、布へ加えた力積と反対の線形・角力積を返す。Engine は Step 成功後に適用する。既定 false。剛体の位置積分後に布を解く時間分割方式で、反作用による剛体の位置変化は次の固定更新。位置の連立解法ではなく、位置の押し出しは布側のみ。軸固定は剛体の通常 API に従う。
- 別 Cloth 間は双方の `interCollisionDistance` を正値 ([0.000001,100] m) にして有効化する。各 Step 後に質点球同士を質量比で分離する。最小距離は双方の大きい方。相互の collisionMask と World の行列を満たす組だけを対象とする。空間グリッドで 4 反復、1 反復 200 万候補まで。固定点・完全追従点は動かさず、最大移動距離を各反復の最後に再適用する。
- 別 Cloth の面–質点・辺–辺と相互 CCD は冒頭の追加設定で有効化する。相互摩擦は対象外。相互衝突は外部接触・自己衝突の後なので、その補正で外部形状へ侵入する場合がある。失敗時は全参加者を相互衝突呼び出し前へ戻すが、成功済みの個別 Step と剛体への力積は取り消さない。
- リファレンス: [Filament Cloth IBL](https://google.github.io/filament/main/filament.html#lighting/imagebasedlights/cloth)、[PBD §4.1 / Appendix A](https://matthias-research.github.io/pages/publications/posBasedDyn.pdf)、[XPBD](https://matthias-research.github.io/pages/publications/XPBD.pdf)、[Bridson et al. Contact / Friction](https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf)、[NVIDIA Inter-Collision](https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#inter-collision)。対応する実装にも `@see` を記載する。

検証 (2026-09-20): Cloth 関連 74 件すべて合格 (`build/agent/test-20260920-003229-52048.log`)。追加 11 件で符号付き曲げ・静止角・面内伸縮・微小三角形、接触の線形/角力積収支、回転面の摩擦、相互衝突の質量比・固定点・マスク・不正入力、World のレイヤー行列、Transform 駆動面の速度、祖先剛体への反作用を検証した。既存のシーン往復テストには二面角・マスク最上位 bit・反作用・相互距離を追加。変更 C++ の個別コンパイル、AgentLint、Physics / Engine / Editor テスト用ビルドは成功。ビルド競合は SDK 完了後の再実行で解消した。Cloth シェーダーは両コピーの VSMain / PSMain を DXC SM 6.8 でコンパイルし、コピー一致を確認。ClothSmoke に新設定の有効化を追加したが、シナリオ実行・実画面・GPU 時間の検証は未実施。Editor 本体への反映は通常のビルドと再起動で行う。

## 最大移動距離ブラシと低解像度転写 (2026-09-19)

- Scene View の `Paint Cloth Pins` を開き、`Max Distance` に切り替える。`Distance (m)` を左ドラッグで設定、Shift+ドラッグで上書きを解除する。物理質点への透過選択で、青が 0、金色がブラシ値以上、灰色が拘束なし、白い輪が固定点。ストローク中はプレビューのみで、確定 1 回につき Undo 1 件。選択・形状・設定・Scene・Play 状態の変更で中断する。
- `ClothComponent.maxDistances` は `{particle, distance}` の疎な上書きリスト。シーン保存・複製・Undo/Redo に対応する。スキン布の未指定点は `skinMaxDistance` を継承する。非スキン布は指定点だけ、布 Transform で移動する静止位置を中心に拘束する。単位はワールド m、0 は完全追従。固定点と明示 `attachments` はブラシ値より優先する。重複・範囲外・非有限値・負の距離は固定更新を止め、修正後に再開する。
- Operator `cloth.paint_max_distance` は `node` (省略時は選択中)、`particles` (空白区切りの物理質点番号)、`distance` (0～100 m、既定 0.1)、`inherit` (既定 false) を受ける。共有 `.cloth` は変更しない。
- 低解像度モデルを先に `cloth.export_mesh` で `.cloth` に書き出す。次に高解像度の静的・スキンメッシュを選択し、同じ操作の `simulationAsset` に低解像度 `.cloth` のパスまたは `guid:`、`maxBindDistance` に許容距離 (モデル空間 m、既定 0.1) を指定する。新規 `.cloth` が生成され、既存アセットは上書きしない。スキンの `submesh` はローカルスロット番号。
- 両形状は同じモデル空間で用意する。スキン描画メッシュは referencePose を焼く。物理形状・固定点・ボーンウェイトは低解像度アセットを引き継ぎ、描画形状・UV・頂点色・seam は高解像度側を保持する。自動メッシュ簡略化・モーフ・別トポロジーへのスキンウェイト推定は行わない。
- v3 の `simulation_indices` は独立した物理三角形、`render_bindings` は描画頂点ごとの `{particles=[3], barycentric=[3], offset=[3]}`。転写時の `render_to_particle` は空。従来の直接対応では新規配列を空にし、v1 / v2 も従来どおり読み込む。
- バインド時に最近接三角形上の重心座標と直交フレーム内のオフセットを保存する。辺方向・面内直交方向・法線の順で、オフセットはモデル空間 m。描画時は補間した質点をローカル空間へ戻して重心補間し、変形後のフレームでオフセットを回転する。潰れた三角形では静止フレームを使い、NaN を防ぐ。法線・接線・境界は従来どおり再計算し、影・深度・選択にも同じ変形メッシュを使う。
- 結合は描画頂点数 × 物理三角形数の最大 2000 万候補まで。距離外・不正形状・上限超過は全体を拒否し、出力は未変更。同距離では先の三角形を選ぶので決定的。ただし近接した別の布層を自動で区別しないため、重なった衣服は分けて書き出す。高解像度の細部自体は衝突判定に参加しない。
- リファレンス: [NVIDIA Motion Constraints](https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints)、[Ericson, Real-Time Collision Detection](https://realtimecollisiondetection.net/) 5.1.5。対応実装にも `@see` を記載する。

検証 (2026-09-19): Cloth 関連 63 件が合格 (`build/agent/test-20260919-234225-38080.log`)。新規 9 件で静的布・スキン布の距離制限、ブラシの確定・中断・継承解除と Undo/Redo、三角形の回転・伸縮への転写、破損データの拒否、v2 スキン互換、物理 3 質点 / 描画 4 頂点の固定更新→描画補間→境界更新を検証した。既存テストも v1 読み込み・距離リストのシーン往復・Editor 経由の結合書き出しを拡張した。個別コンパイルと AgentLint は成功。Engine / Editor のテスト用ビルド完了後に実行した。ClothSmoke は移動後のシーンパスを修正し、距離設定→Play→上書き解除を追加したが、Editor 本体でのシナリオ実行・実画面確認は未実施。反映には Editor 本体のビルドと再起動が必要。

## 複数ボーン・自己衝突の拡張契約

- ClothAsset v2 はボーン名と逆バインド行列、質点ごとの元頂点位置・最大 4 ウェイトを保存する。静止形状は referencePose を焼いた位置。v1 の読み込みは維持し、曖昧なボーン名、範囲外・負・非有限・総和 0 のウェイト、seam の不一致を拒否する。入力ウェイトは同一ボーンを集約して正規化する。
- ClothComponent の useSkinning を有効にし、skinTarget に元の SkinnedMeshRenderer を持つ GameObject を設定する。名前でその Skeleton / nodeEntities と対応させ、Bone world × inverseBind × 元頂点位置を線形ブレンドした中心に skinMaxDistance の移動制約をかける。固定質点は半径 0、明示 attachments は該当質点を上書きする。時間契約は既存の単一ボーン追従と共通。モーフ・異なるトポロジーへの最近傍ウェイト転写は対象外。
- Editor の cloth.export_mesh は静的・スキン両方に対応し、submesh 引数は SkinnedMeshRenderer のローカルスロット番号。布の GameObject は skinTarget と同じモデル空間に配置し、作成アセットと skinTarget を設定する。
- 自己衝突は単一 Cloth 内の質点球同士の離散 PBD。selfCollisionDistance=0 なら無効、有効範囲は [0.000001, 100] m。selfCollisionStiffness は [0, 1]、既定 1、0 なら押し出さない。距離・曲げ拘束でつながった組と、初期距離が設定距離より小さい組を除外する。逆質量で押し出しを配分し、完全固定は動かさない。セル幅を接触距離とする空間グリッドを反復ごとに再構築し、セル・質点順を固定して再現性を持たせる。1 回の探索が 200 万候補を超えたら失敗として Step 全体を巻き戻す。
- 自己衝突は距離拘束の後、移動制約・外部接触の前に解く。反復回数による近似で、移動範囲・外部接触が優先。別 Cloth 間の衝突は含めない。接触距離はメッシュの質点間隔に合わせて設定する。面・辺の接触と CCD は次節。
- リファレンス: [Khronos Skinning](https://github.khronos.org/glTF-Tutorials/gltfTutorial/gltfTutorial_020_Skins.html)、[NVIDIA Cloth self-collision](https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#self-collision-of-a-single-cloth-actor)。対応コードにも @see を記載する。

## 面・辺の接触と CCD

- `selfCollisionFaces=true` で、質点球に加えて質点–三角形と辺–辺の近接を `selfCollisionDistance` だけ離す。三角形の内部 (重心座標がすべて正) と辺の内点だけを扱い、頂点・辺の端は質点球に任せる。距離 0 か stiffness 0 なら無効。
- 押し出す向きは現在の面法線 (辺–辺は 2 辺の外積) を、substep 開始時に質点が居た側へ向けたもの。開始時に同一平面で側が決まらない組は補正しない。符号付き距離が (-distance, distance) の組だけを近接として押し出し、それより深く裏へ抜けた組は CCD に任せる。
- 質点・自分を含む三角形、端点を共有する辺、静止形状で既に接触距離より近い組は除外する。固定点だけの組は候補にしない。
- 広域判定は substep 開始位置から予測位置までの掃引 AABB (接触距離の 2 倍で膨張) の x 軸 sort-and-sweep。候補は substep 中固定で、反復ごとに作り直さない。走査が 200 万件を超えるか AABB が非有限なら Step 全体を巻き戻す。整列は x 下端と添字で決め、再現性を持たせる。
- `continuousCollision=true` かつ面接触が有効なら、反復後の最終位置で候補を取り直し、開始→最終の等速直線運動で 4 点が同一平面になる最初の時刻を三次方程式 (double) で求める。その時刻に接触距離内なら、その時刻の重心座標・辺の媒介変数で開始時の側へ完全に戻す。補正が別の交差を生む場合に備えて最大 4 回走査する。substep 開始時に既に交差している状態からは解かない。
- `continuousCollision=true` では球・カプセルとの接触も掃引する。形状は現在位置で与えられるため、形状に固定した座標で開始点を `velocity * h` だけ戻し、膨張半径 (radius + thickness) への最初の進入点へ質点を置く。その後は離散接触と同じ摩擦・法線速度の処理を行う。開始点が既に内側なら従来の離散押し出し。平面は半空間なので掃引しない。
- 移動制約は依然として最後に射影するので、両立しない設定では自己交差・接触面への侵入を許す。
- リファレンス: [Bridson et al. 2002, Robust Treatment of Collisions, Contact and Friction for Cloth Animation](https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf) の 4 Proximity / 6 Collisions、[Müller et al. 2007, Position Based Dynamics](https://matthias-research.github.io/pages/publications/posBasedDyn.pdf) の 4.4、[Ericson, Real-Time Collision Detection](https://realtimecollisiondetection.net/) 5.1.5 / 5.1.9、[Inigo Quilez, Intersectors](https://iquilezles.org/articles/intersectors/) の Sphere / Capsule。

検証 (2026-09-17): ClothSolverTest 22 件 (新規 5 件: 質点–三角形の近接、辺–辺の近接、1 substep で抜ける質点・辺の CCD 有無の対照、CCD の再現性、球・カプセルの掃引の有無の対照) を含む `Fiber|Cloth|RenderPipeline|SceneSerializer` 98 件が合格。既定値はどちらも false のため、既存シーンの挙動は変わらない。実シーンでの画面確認は未実施。Editor 本体への反映にはビルドと再起動が必要 (ClothComponent のレイアウトが変わる)。

## 責務

`Physics/Cloth` は Math と標準ライブラリだけを使う CPU 質点 XPBD。Engine の型、GPU、ファイル形式を知らない。既存の剛体 XPBDSolver は変更しない。
Engine がシーン、固定先、風、Collider、保存、描画を仲介する。Fluid と Physics の間に依存を追加しない。
布の反射は独立した Cloth.hlsl と ClothBRDF.hlsli。静止メッシュにも使用でき、シミュレーション側は任意のマテリアルを使用できる。

## 物理の契約

- 座標はワールド空間 [m]、時間 [s]、質量 [kg]。固定刻みを呼び出し側が管理し、ソルバーは substep に分割する。
- 頂点、速度、三角形、拘束はソルバーが所有する。入力 span は呼び出し中だけ参照する。
- 各辺の距離拘束で伸びを抑える。曲げは旧対頂点距離近似と符号付き二面角から選ぶ。織り方向ごとの材料モデルは未対応。
- compliance は距離拘束では [m/N]、二面角では [1/(N m)]。substep ごとに lambda をゼロにし、その substep の反復中だけ累積する。
- 逆質量 0 は固定点。固定先の移動は substep ごとに補間する。接触は固定点を押し出さない。
- 風は面の相対速度に対する法線方向の抗力。面積と各頂点の質量を反映する。裏面でも風下向きになる。
- 球、カプセル、片側平面との離散接触。接触厚みと相対接線速度の減衰を持つ。自己衝突・CCD・剛体への反作用・別 Cloth の質点衝突は任意で有効化する。
- 不正な入力は false。初期化失敗時は既存状態を維持する。進行中の非有限値はその Step を巻き戻す。Engine 側が Logger に報告する。

## 描画の契約

- Charlie NDF と Neubelt の visibility、Lambert 拡散。両面の法線マップを同じ接線基底で評価してから裏面を反転する。
- 環境光は拡散 irradiance と専用の Charlie 数値積分。GGX 用の粗さ prefilter/DFG LUT を Charlie の積分として使用しない。
- GBuffer は sheen を保持できないため不透明 Forward で描画する。材質の sheen を 0 にしても Cloth モデルは Forward に残す。
- シミュレーション頂点と描画頂点の対応を明示し、UV seam を物理的な裂け目にしない。同位置だけで異なる布を自動溶接しない。
- Engine 統合では専用の変形メッシュを所有し、法線・接線・境界を更新する。影・深度・選択もそのメッシュを使う。

## 導入段階

1. 物理単体とテスト、ClothShader と標準マテリアル。
2. ClothComponent / 固定更新 / 描画転送 / 自動 Inspector と保存。旗・カーテンを対象とする。
3. ClothAsset、固定点編集、Playtest と基準画像。
4. ボーン追従、最大移動距離、衣服向け接触。LateUpdate の最終ボーン姿勢と固定刻みの時間契約を先に決める。
5. 自己衝突、低解像度からの転写、計測後の GPU 化。

## 初期実装の操作と制限

- 空の GameObject に Physics → Cloth を追加する。ローカル XY 平面に布が生成され、原点は上辺中央。MeshRenderer は自動で付く。
- カーテンは pinTop=true。旗は pinTop=false / pinLeft=true。windVelocity はワールド空間 [m/s]。materialPath を空にすると既存の Material を使用する。
- 同じ GameObject に別の動的メッシュ生成 Component を併用しない。Cloth が MeshRenderer の形状を所有する。
- Inspector の編集状態では静止形状を表示する。Play で固定更新、Pause で停止。サイズ・分割数・面密度・固定辺・スケールの変更と teleportDistance 超の移動で再初期化する。
- 正の非ゼロスケールのみ対応。現行バックエンドは DX12 / SM 6.8 (DX11 は既に廃止済み)。
- 接触対象はシーンの有効な非 Trigger Sphere/Capsule と任意の水平 groundHeight。レイヤー・移動面速度・剛体への反作用は冒頭の拡張契約を参照。
- clothAssetPath が空なら格子、指定時は `.cloth` の任意三角形メッシュを使用する。アセット参照は `guid:`。アセットの形状・対応表・固定点を再読み込みすると初期状態に戻る。
- overridePins=true にすると Inspector の pinnedParticles リストが固定点を置き換える。particle は 0 始まりの**物理質点番号**。リストが空なら固定なし。格子では `y * (segments + 1) + x`、アセットでは particles の添字。重複はまとめ、範囲外はエラーとして停止する。変更はシーン保存・Play/Stop・Undo の反射経路に載る。
- pinTop / pinLeft は格子専用。`.cloth` 使用時はアセットの pins または上書きリストを使う。Scene View の固定点ブラシでも個体ごとの上書きリストを編集できる。
- 二面角拘束は dihedralBending を有効化する。環境 sheen は Cloth マテリアルの sheen 色と環境光設定に従う。

## 検証用データ

`GreenWare/Assets/Scenes/Test/ClothDemo.scene` に旗・カーテン・接触球を置く。起動確認は `GreenWare/Tests/Playtests/ClothSmoke.playtest.json`。物理的な変形と固定点は ClothSolverTest / ClothSystemTest で数値検証する。基準画像は実機で見た目を確認してから追加する。

変更ファイルの個別コンパイルは `Tools/AgentBuild.ps1 check`、テストは `build FBZZTestsPhysicsAuto` / `build FBZZTestsEngineAuto` の後に `test -Filter Cloth`。共有 build ツリーへの同時ビルドは obj をロックするため直列に実行する。

2026-09-17: ClothSolverTest 9 件、ClothSystemTest 7 件、ClothAssetTest 6 件の計 22 件が合格。Engine のテスト用ビルド、Editor の書き出し操作の個別コンパイル、変更した布コードの AgentLint が成功。DXC で Cloth.hlsl の VSMain / PSMain を SM 6.8 としてコンパイル済み。描画の基準画像検証は未実施。

## リファレンス

- XPBD、式 (18)–(19): https://matthias-research.github.io/pages/publications/XPBD.pdf
- Small Steps: https://matthias-research.github.io/pages/publications/smallsteps.pdf
- Cloth BRDF: https://google.github.io/filament/main/filament.html#materialsystem/cloth

対応する実装にも `@see` でリンクを残す。

## ClothAsset v1 / v2 / v3

`Engine/Asset/ClothAsset` が CPU データと TOML 入出力、`Engine/Format/ClothFormat.hpp` がバージョンと上限を持つ。Physics にファイルや描画頂点を持ち込まない。AssetManager の共有キャッシュ、GUID 解決、失敗キャッシュの解除、ホットリロード、UnloadAll に登録する。

- `version = 1`。`particles` はローカル座標 xyz、`vertices` は position xyz / normal xyz / tangent xyz / uv / color rgba の 15 要素配列。
- 現行の保存は `version = 3`。v2 で追加した `skin_bones` は name と行優先 16 要素の inverse_bind、`skin_weights` は元座標 position (xyz)、bones (4 添字)、weights (4 値)。静的布では両方空。スキンの場合は質点ごとに 1 レコード、ボーン名は一意、上限 128。v1 はスキン情報なしとして読む。v3 の転写用配列は冒頭の節を参照。
- `indices` は描画頂点の三角形インデックス。`render_to_particle` は描画頂点ごとの物理質点 ID。物理の三角形はこの対応表から導出する。
- `pins` は固定する物理質点 ID。未使用質点、非有限値、退化面、非多様体辺、範囲外・重複した固定点を拒否する。対応する描画・物理頂点は位置が 1 μm 以内で一致すること。
- 上限は描画/物理頂点それぞれ 65536、三角形インデックス 393216。読み込み失敗は出力を変更せず、保存失敗は既存ファイルを保持する。
- `CreateClothAsset(mesh, mapping, out)` は任意の静的 CPU メッシュから生成する。mapping 未指定は 1:1。明示した同じ質点 ID だけが継ぎ目を共有し、同位置による自動溶接はしない。UV・頂点色・描画トポロジーは保持する。`CreateSkinnedClothAsset` はスキンメッシュを referencePose の静止形状へ変換し、元座標・ウェイト・逆バインドも保存する。同じ質点へ割り当てる頂点の元座標と正規化後ウェイトが一致しなければ拒否する。
- シミュレーションは質点数で動き、描画転送は対応表を使う。質量も物理三角形の面積から集計する。各描画頂点の法線・接線は描画トポロジーで再計算するため、分離した seam のシェーディングは共有しない。

Editor の操作 `cloth.export_mesh`（パレット表示名 `Export Selected Mesh as Cloth`）で選択中の MeshRenderer または SkinnedMeshRenderer を `Assets/Cloth/<一意名>.cloth` に保存する。スキンでは `submesh` 引数（既定 0）でローカルスロットを指定する。ClothComponent 追加前に実行し、作成アセットの GUID を clothAssetPath に設定する。既存アセットは上書きしない。書き出し時の対応は 1:1 で、seam をつなぐ場合は API の mapping または `.cloth` の対応表を明示する。共有アセットの pins はファイル、個体ごとの固定点は Inspector の overridePins / pinnedParticles で編集する。

スキン布は元の Renderer と同じモデル空間（例: 元 GameObject の子でローカル TRS は既定値）に Cloth を置く。`Skin Weights` の `useSkinning` を有効にし、`skinTarget` に元の Renderer、`skinMaxDistance` に自由な質点の最大移動距離 [m] を設定する。ボーン参照は描画の enabled とは独立して評価する。二重描画になる場合は元の衣服メッシュの描画を無効にする。自己衝突は `Collision / selfCollisionDistance` を正の値にして有効化する。

`GreenWare/Assets/Cloth/TaperedCurtain.cloth` は 81 質点・90 描画頂点で、中央に明示的な seam を持つ先細り形状。ClothDemo の Curtain が使用する。Flag は格子のままで両経路を確認できる。低解像度転写は冒頭の節を参照。

## 固定点ブラシ

ClothComponent を持つオブジェクトを選び、Scene View 下部の `Paint Cloth Pins` または同名のパレット操作を実行する。左ドラッグで固定、Shift を押してドラッグ開始すると解除。Radius は画面上のピクセル半径。金色が固定点、水色が自由な点。透過選択なので、奥の点や他の物体に隠れた点も円の範囲なら対象になる。必要に応じて視点を変えて塗る。

ドラッグ中は候補のみを表示し、Scene View 内で離したとき `cloth.paint_pins` を一度呼ぶ。1 ストロークが 1 Undo で、Undo は元の overridePins とリストをそのまま復元する。初回編集ではアセット/格子の固定点を引き継ぐ。変化のない操作は継承設定も Undo 履歴も変更しない。共有 `.cloth` は変更せず、結果はシーンの ClothComponent に保存する。

Esc、Alt/Ctrl、右/中ボタン、選択/シーン/形状/固定点の変更、Play/Pause 移行で候補を中断する。ウィンドウ外で離した場合も確定しない。ブラシ使用中は通常のクリック選択、矩形選択、Transform ギズモ、頂点/面スナップを抑制する。地形ツール・Map 編集とは併用しない。

AI バスからも登録済み Operator `cloth.paint_pins` を使用する。引数は `node`（省略時は選択中）、`particles`（0 始まり物理質点番号の空白区切り文字列）、`pin`（既定 true）。範囲外・不正なトークンは一括拒否し、一部だけ適用しない。ランタイム初期化完了後、編集モードでのみ実行する。

固定点ブラシの検証 (2026-09-17): Editor テスト用ビルド成功。ClothPinBrushTest 8 件 (マウス入力による確定・Play 中断、Undo/Redo、継承設定復元、不正入力、形状変更、シーン切り替え) を含む Cloth 関連 30 件が合格。個別コンパイルと AgentLint も成功。実際の Scene View の画面確認は未実施。

## ボーン追従と最大移動距離（段階 4 の前半）

Inspector の `Bone Attachments / attachments` に質点単位の設定を追加する。`particle` は物理質点番号、`target` は同一シーンの Bone GameObject（通常の GameObject も可）、`localPosition` は追従先のローカル位置。`maxDistance` はワールド空間 [m] の球半径で、0 は完全追従、正の値は球内での自由な揺れを許す。追従先のスケールは中心位置に反映するが、半径は変更しない。設定はシーン保存・Play/Stop の反射経路に載る。共有 `.cloth` の形式は変えない。

固定点ブラシやアセットで固定した質点も半径 0 なら追従先が優先される。正半径にしたい質点は先に固定を解除する。固定質点への正半径、同じ質点の重複、範囲外、無効・非アクティブな参照先、非有限値・負半径を拒否してその固定更新を止める。正しい設定に戻せば再開する。リストから削除すると元の質量・固定点設定に戻る。

### 時間契約

`Physics` の ClothSystem は `PhysicsSystem` の後に実行する。Animator → IK → SpringBone はその後の `LateUpdate` で Bone の local/world Transform とスキニング行列を確定する。次フレームの `PrePhysics` が階層の world Transform を更新し、ClothSystem はその時点の Bone Transform を読む。つまりアニメーションは直前に完了した LateUpdate の姿勢で、親やスクリプトの当フレーム移動は PrePhysics 後の座標に反映される。初回はシーンの初期姿勢を使う。アニメーションを先読みせず、既存システムの Phase も変更しない。このため描画される当フレームのスキン姿勢と布には遅れがある。

成功した固定更新の中心を保存し、次の中心まで substep で線形補間する。同一フレーム内で複数回固定更新する場合、同じ target Transform なら 2 回目から中心は静止する。初回・設定変更・布の再初期化時は、完全追従の開始点を現在の質点位置、正半径の開始中心を新しい中心とする。Pause は状態・中心履歴を進めない。編集モードでは従来どおり静止形状を表示する。

### ソルバーと接触の優先順位

`Physics/Cloth` は Entity や Bone を知らず、`ClothMotionConstraint` の前回中心・今回中心・半径を受け取る。半径 0 の質点は当該 Step だけ有効逆質量を 0 にし、距離拘束で動かさず、他の質点へ拘束を伝える。距離拘束の各反復と接触解決の後に球へ射影する。移動範囲を接触より優先するため、球内に非貫通位置がない設定では Collider への侵入を許す。移動面速度・衝突レイヤーは冒頭の拡張契約、CCD は「面・辺の接触と CCD」節を参照。

参照: [NVIDIA Cloth Motion Constraints](https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints)。球内への射影という方式のリファレンスとして使用し、ライブラリ依存は追加しない。単一ボーン追従の後に複数ボーンとスキン書き出しを追加した。最大距離のブラシ編集は冒頭の節を参照。

検証 (2026-09-17): ClothSolverTest 13 件、ClothSystemTest 10 件、ClothAssetTest 6 件、ClothPinBrushTest 8 件の計 37 件が合格 (`build/agent/test-20260917-171514-28708.log`)。Physics / Engine / Editor のテスト用ビルドと変更 6 ファイルの AgentLint が成功。中心補間、固定解除時の質量復帰、衝突と移動範囲の競合、不正入力の一括拒否、追従先の位置・回転・スケール、Pause、GUID 参照のシーン往復を検証した。実際のアニメーション付き衣服の画面確認は未実施。

複数ボーン・自己衝突の検証 (2026-09-17): ClothSolverTest 17 件、ClothSystemTest 12 件、ClothAssetTest 11 件、ClothPinBrushTest 9 件の計 49 件が合格 (`build/agent/test-20260917-181541-49784.log`)。Physics / Engine / Editor のテスト用ビルド、変更 C++ ファイルの個別コンパイルと AgentLint が成功。ウェイトの正規化・逆バインド行列・ボーン名対応・v1 互換読み込み・v2 保存往復・スキン書き出し操作・明示追従の優先と解除・自己衝突の分離と除外・固定点の保持・不正入力を検証した。実際のアニメーション付き衣服の画面確認は未実施。Editor 本体への反映にはビルドと再起動が必要。
