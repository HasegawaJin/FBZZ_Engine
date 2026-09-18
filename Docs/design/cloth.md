# Cloth — シミュレーションと布の反射

作成日: 2026-09-17。段階的に導入する。ユーザー承認済みの責務分割を正本とする。

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
- 各辺の距離拘束で伸びを抑える。初期の曲げは隣接三角形の対頂点間距離による近似。二面角や織り方向ごとの材料モデルではなく、大きな折れ曲がりへの精度には限界がある。
- compliance は距離拘束に対する [m/N]。substep ごとに lambda をゼロにし、その substep の反復中だけ累積する。
- 逆質量 0 は固定点。固定先の移動は substep ごとに補間する。接触は固定点を押し出さない。
- 風は面の相対速度に対する法線方向の抗力。面積と各頂点の質量を反映する。裏面でも風下向きになる。
- 球、カプセル、片側平面との離散接触。接触厚みと接線速度の減衰を持つ。質点・面・辺の自己衝突と CCD は任意で有効化する。剛体への反作用は未対応。
- 不正な入力は false。初期化失敗時は既存状態を維持する。進行中の非有限値はその Step を巻き戻す。Engine 側が Logger に報告する。

## 描画の契約

- Charlie NDF と Neubelt の visibility、Lambert 拡散。両面の法線マップを同じ接線基底で評価してから裏面を反転する。
- 初期の環境光は拡散 irradiance のみ。GGX 用 prefilter/DFG LUT を Charlie の積分として使用しない。環境 sheen は後続段階で専用積分を追加する。
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
- 接触対象はシーンの有効な非 Trigger Sphere/Capsule と任意の水平 groundHeight。初期版では衝突レイヤーマスク、動く面の摩擦速度、剛体への反作用は未対応。
- clothAssetPath が空なら格子、指定時は `.cloth` の任意三角形メッシュを使用する。アセット参照は `guid:`。アセットの形状・対応表・固定点を再読み込みすると初期状態に戻る。
- overridePins=true にすると Inspector の pinnedParticles リストが固定点を置き換える。particle は 0 始まりの**物理質点番号**。リストが空なら固定なし。格子では `y * (segments + 1) + x`、アセットでは particles の添字。重複はまとめ、範囲外はエラーとして停止する。変更はシーン保存・Play/Stop・Undo の反射経路に載る。
- pinTop / pinLeft は格子専用。`.cloth` 使用時はアセットの pins または上書きリストを使う。Scene View の固定点ブラシでも個体ごとの上書きリストを編集できる。
- 初期曲げは対頂点距離の近似。二面角拘束、環境 sheen は未実装。

## 検証用データ

`GreenWare/Assets/Scenes/ClothDemo.scene` に旗・カーテン・接触球を置く。起動確認は `GreenWare/Tests/Playtests/ClothSmoke.playtest.json`。物理的な変形と固定点は ClothSolverTest / ClothSystemTest で数値検証する。基準画像は実機で見た目を確認してから追加する。

変更ファイルの個別コンパイルは `Tools/AgentBuild.ps1 check`、テストは `build FBZZTestsPhysicsAuto` / `build FBZZTestsEngineAuto` の後に `test -Filter Cloth`。共有 build ツリーへの同時ビルドは obj をロックするため直列に実行する。

2026-09-17: ClothSolverTest 9 件、ClothSystemTest 7 件、ClothAssetTest 6 件の計 22 件が合格。Engine のテスト用ビルド、Editor の書き出し操作の個別コンパイル、変更した布コードの AgentLint が成功。DXC で Cloth.hlsl の VSMain / PSMain を SM 6.8 としてコンパイル済み。描画の基準画像検証は未実施。

## リファレンス

- XPBD、式 (18)–(19): https://matthias-research.github.io/pages/publications/XPBD.pdf
- Small Steps: https://matthias-research.github.io/pages/publications/smallsteps.pdf
- Cloth BRDF: https://google.github.io/filament/main/filament.html#materialsystem/cloth

対応する実装にも `@see` でリンクを残す。

## ClothAsset v1 / v2

`Engine/Asset/ClothAsset` が CPU データと TOML 入出力、`Engine/Format/ClothFormat.hpp` がバージョンと上限を持つ。Physics にファイルや描画頂点を持ち込まない。AssetManager の共有キャッシュ、GUID 解決、失敗キャッシュの解除、ホットリロード、UnloadAll に登録する。

- `version = 1`。`particles` はローカル座標 xyz、`vertices` は position xyz / normal xyz / tangent xyz / uv / color rgba の 15 要素配列。
- 現行の保存は `version = 2`。`skin_bones` は name と行優先 16 要素の inverse_bind、`skin_weights` は元座標 position (xyz)、bones (4 添字)、weights (4 値)。静的布では両方空。スキンの場合は質点ごとに 1 レコード、ボーン名は一意、上限 128。v1 はスキン情報なしとして読む。
- `indices` は描画頂点の三角形インデックス。`render_to_particle` は描画頂点ごとの物理質点 ID。物理の三角形はこの対応表から導出する。
- `pins` は固定する物理質点 ID。未使用質点、非有限値、退化面、非多様体辺、範囲外・重複した固定点を拒否する。対応する描画・物理頂点は位置が 1 μm 以内で一致すること。
- 上限は描画/物理頂点それぞれ 65536、三角形インデックス 393216。読み込み失敗は出力を変更せず、保存失敗は既存ファイルを保持する。
- `CreateClothAsset(mesh, mapping, out)` は任意の静的 CPU メッシュから生成する。mapping 未指定は 1:1。明示した同じ質点 ID だけが継ぎ目を共有し、同位置による自動溶接はしない。UV・頂点色・描画トポロジーは保持する。`CreateSkinnedClothAsset` はスキンメッシュを referencePose の静止形状へ変換し、元座標・ウェイト・逆バインドも保存する。同じ質点へ割り当てる頂点の元座標と正規化後ウェイトが一致しなければ拒否する。
- シミュレーションは質点数で動き、描画転送は対応表を使う。質量も物理三角形の面積から集計する。各描画頂点の法線・接線は描画トポロジーで再計算するため、分離した seam のシェーディングは共有しない。

Editor の操作 `cloth.export_mesh`（パレット表示名 `Export Selected Mesh as Cloth`）で選択中の MeshRenderer または SkinnedMeshRenderer を `Assets/Cloth/<一意名>.cloth` に保存する。スキンでは `submesh` 引数（既定 0）でローカルスロットを指定する。ClothComponent 追加前に実行し、作成アセットの GUID を clothAssetPath に設定する。既存アセットは上書きしない。書き出し時の対応は 1:1 で、seam をつなぐ場合は API の mapping または `.cloth` の対応表を明示する。共有アセットの pins はファイル、個体ごとの固定点は Inspector の overridePins / pinnedParticles で編集する。

スキン布は元の Renderer と同じモデル空間（例: 元 GameObject の子でローカル TRS は既定値）に Cloth を置く。`Skin Weights` の `useSkinning` を有効にし、`skinTarget` に元の Renderer、`skinMaxDistance` に自由な質点の最大移動距離 [m] を設定する。ボーン参照は描画の enabled とは独立して評価する。二重描画になる場合は元の衣服メッシュの描画を無効にする。自己衝突は `Collision / selfCollisionDistance` を正の値にして有効化する。

`GreenWare/Assets/Cloth/TaperedCurtain.cloth` は 81 質点・90 描画頂点で、中央に明示的な seam を持つ先細り形状。ClothDemo の Curtain が使用する。Flag は格子のままで両経路を確認できる。低解像度転写は後続段階。

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

`Physics/Cloth` は Entity や Bone を知らず、`ClothMotionConstraint` の前回中心・今回中心・半径を受け取る。半径 0 の質点は当該 Step だけ有効逆質量を 0 にし、距離拘束で動かさず、他の質点へ拘束を伝える。距離拘束の各反復と接触解決の後に球へ射影する。移動範囲を接触より優先するため、球内に非貫通位置がない設定では Collider への侵入を許す。接触形状は従来の Sphere/Capsule/平面で、移動面速度、衝突レイヤー対応は後続。CCD は「面・辺の接触と CCD」節。

参照: [NVIDIA Cloth Motion Constraints](https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints)。球内への射影という方式のリファレンスとして使用し、ライブラリ依存は追加しない。単一ボーン追従の後に複数ボーンとスキン書き出しを追加した。最大距離のブラシ編集は未対応。

検証 (2026-09-17): ClothSolverTest 13 件、ClothSystemTest 10 件、ClothAssetTest 6 件、ClothPinBrushTest 8 件の計 37 件が合格 (`build/agent/test-20260917-171514-28708.log`)。Physics / Engine / Editor のテスト用ビルドと変更 6 ファイルの AgentLint が成功。中心補間、固定解除時の質量復帰、衝突と移動範囲の競合、不正入力の一括拒否、追従先の位置・回転・スケール、Pause、GUID 参照のシーン往復を検証した。実際のアニメーション付き衣服の画面確認は未実施。

複数ボーン・自己衝突の検証 (2026-09-17): ClothSolverTest 17 件、ClothSystemTest 12 件、ClothAssetTest 11 件、ClothPinBrushTest 9 件の計 49 件が合格 (`build/agent/test-20260917-181541-49784.log`)。Physics / Engine / Editor のテスト用ビルド、変更 C++ ファイルの個別コンパイルと AgentLint が成功。ウェイトの正規化・逆バインド行列・ボーン名対応・v1 互換読み込み・v2 保存往復・スキン書き出し操作・明示追従の優先と解除・自己衝突の分離と除外・固定点の保持・不正入力を検証した。実際のアニメーション付き衣服の画面確認は未実施。Editor 本体への反映にはビルドと再起動が必要。
