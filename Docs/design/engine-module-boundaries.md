<!-- @file    engine-module-boundaries.md -->
<!-- @brief   Scene ストレージ、native host と配布 SDK の公開境界。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-30 -->
# Engine の内部構造と公開境界

2026-10-30 に、内部の C++ 利用箇所も更新して公開範囲を絞る方針を選択した。C# SDK、アセット形式、EntityID、Component の安定アドレス、Scene の保存順は維持する。

Scene の ComponentArray tuple は非公開の ComponentStorage が所有する。Scene のヘッダーは Component の具体定義を読み込まず、型付きの配列アクセスだけを実装側へ接続する。登録は一つの一覧から前方宣言、メタデータと配列アクセスを作り、別々の型リストを手で保守しない。利用側は使う Component のヘッダーを明示的に読む。Inspector と Serializer の登録メタデータはストレージの公開レイアウトにしない。

描画要求の資源アクセス型と挿入点は軽量な宣言ヘッダーへ分ける。Scene は要求を保持し、実際の RenderPassContext と GPU 定数を読み込まない。描画 callback の既存契約は [graphics-library.md](graphics-library.md) に従う。

native DLL の境界は各ライブラリの API マクロで明示する。ヘッダーに宣言された外部関数・メソッド・共有データを対象とし、ソースだけの補助処理や STL の暗黙インスタンスを自動 export しない。Editor とテストが使う内部 C++ 接続もマクロを付けるが、それをゲーム作者向け API として配布しない。異なる版・コンパイラー・CRT の DLL 混在はサポートしない。

ゲーム作者向け Engine native ヘッダーは Standalone の起動入口と既存の managed POD ABI に絞る。起動処理は Engine が所有し、GameHub のテンプレートは起動入口だけを呼ぶ。Math・Physics・Fluid・Core・Graphics の独立ライブラリは引き続きそれぞれの公開ヘッダーを配布する。ゲームロジックの入口は C# SDK とする。

明示 export に移行したライブラリの LTO は通常の IPO 設定を使い、過去の自動 export の一覧や STL 補助シンボルを凍結して維持しない。SDK の検証は、必要な起動ヘッダーと managed ABI の存在、内部 Engine ヘッダーの非配布、テンプレートの利用境界を検査する。

ホストは ResourceManager、AssetSession、Scene／実行モジュールの順に所有する。破棄時は逆順となり、Scene の解放後にアセットの非同期候補と共有キャッシュを終了し、最後に ResourceManager を破棄する。初期化失敗と早期 return でも AssetSession が同じ順序を守る。

GPU の動的バッファプールは単独所有・move のみとし、切り替え・代入・破棄時に記録した作成元へ返す。現在 Active な Manager を解放先として使わない。終了済みの所有者と Reset 済みの世代は解決しない。UI、skinning、instance、fiber のキャッシュも作成元と世代を照合してから置き換える。managed runtime texture の Dispose／世代終了も同じ所有者解決を使う。

地形抽出の GPU キャッシュは Scene 世代ごとに持ち、その中で ResourceManager の寿命 ID と Reset 世代を分離する。複数 Scene と複数 Manager を交互に描画しても、別の地形のデータを再利用したり、生存中の作成元の資源を見失ったりしない。Scene の Clear／破棄で全作成元へ、Entity／TerrainComponent の削除でその個体の資源だけを返す。高さ・スプラットの更新はキャッシュごとの revision と照合し、最初の抽出が dirty を消しても他の作成元へ反映する。キャッシュの所有と終了処理は内部の TerrainRenderCache に閉じ、抽出処理から分離する。

Engine の内部ターゲットは Platform、Assets、Audio、AI、Scene、Rendering、Managed の OBJECT モジュールとする。Scene の走査・抽出とホスト描画アダプターは EngineRendering、GPU のパス実装と資源管理は引き続き Graphics が所有する。Asset の Assimp／画像デコード、Scene の高さマップ読込み、Managed の .NET host ヘッダーはそれぞれの PRIVATE 依存へ閉じる。FBZZEngine DLL は Application 等の合成ルートと各 OBJECT を一度ずつ取り込み、自己登録 TU を維持する。configure で未所属と重複所属を拒否する。内部モジュールは独立ライブラリの ABI ではなく、Engine 内の責務・コンパイル依存の境界である。

Script のアセット要求台帳と先読み転送は ScriptAssetProxy.cpp へ閉じ、他の Component プロキシの実装から分離する。公開 C# SDK と managed POD ABI は変更しない。

水面も地形と同じく Scene 世代・ResourceManager の寿命 ID・Reset 世代を組にしてキャッシュを所有する。Manager の切り替えで他の所有者のキャッシュを捨てず、Scene の終了と WaterComponent の削除で全所有者へ返す。共有の dirty フラグを消す前に各キャッシュへ無効化を伝え、先に描画したビューが更新を消費しても他の所有者のメッシュ・テクスチャを更新する。終了済み・Reset 済みの所有者へ古いハンドルを返さない。

速度場アトラスは ResourceManager の寿命 ID・Reset 世代ごとに単独所有する。Manager の切り替えでタイル・現在のテクスチャ・退役中のテクスチャを捨てない。アセットの無効化は全所有者へ伝え、旧テクスチャは既存の3フレーム退役期間を守って各作成元へ返す。描画終了後の Reset／AssetSession 終了では全所有者の現在・退役資源を返す。終了済み・Reset 済みの所有者は解決せず、別所有者の資源へ触れない。検証は VelocityFieldAtlasOwnershipTest で行う。

残像の材質キャッシュは Scene 世代・ResourceManager の寿命 ID・Reset 世代ごとに持つ。Scene の終了で独自のパラメーターバッファを全作成元へ返し、共有シェーダーは Manager に残す。Component の色・サンプルのボーンバッファは実体の有効性を確認し、描画先が変われば旧作成元へ返して再確保する。ボーン姿勢は再確保時に dirty 状態によらず転送する。シェーダー再読込みでパラメーターが消えた場合も旧バッファを返す。検証は MeshTrailRenderResourceTest で行う。

粒子の GPU シミュレーション用バッファ・定数・ソート資源は Component が所有し、Clear、Restart、容量変更、作成元／Reset 世代変更で元の Manager へ返してから再作成する。spawn／force のフレームプールは借用なので返却しない。描画定数も実体の作成元を照合する。材質パラメーターは Scene 世代・Manager 寿命 ID・Reset 世代のキャッシュが所有し、Scene 終了で全所有者へ返す。共有シェーダーは Manager に残す。検証は ParticleGpuOwnershipTest で行う。

粒子の時刻・Delay・Loop・Burst・距離発生・Prewarm は Scene 内部の ParticlePlayback で一度だけ定義する。通常再生はフレームガード経由、編集スクラブは固定刻みで同じ更新処理を呼ぶ。スクラブは整数フレームと端数に分け、累積 float の誤差で最後の刻みと連続発生量を短くしない。duration が 0 でも再生時刻を進め、正の時刻の Burst を発火する。検証は ParticlePlaybackTest と既存の ParticleLoopInspectionTest で行う。

FluidGpuSolver、FluidGpuLiquidSolver、VolumeFlipbookBaker の作成した GPU 資源は Graphics の move 専用 OwnedResource で単独所有する。破棄・再代入は各ハンドルの作成元へ返し、ロードした共有シェーダーは借用する。Manager 寿命 ID と Reset 世代を初期化時に照合して再構築し、異なる作成元での Step／Write は記録しない。進行中ベイクの作成元変更は失敗として再開始を要求する。プレビューは所有者変更でキャッシュと子ソルバーを再構築する。検証は BakeGpuOwnershipTest で行う。

AnimatorControllerAsset は AnimatorControllerDefinition の保存用値型だけを含む。Component の再生時刻、BlendTree の平滑化／位相、Mask の読込みキャッシュ、Slot の再生要求を共有アセットに保持しない。Make／Apply が保存対象を明示変換し、同じ Controller の編集反映では既存の再生ステート時間を維持する。.animcontroller のキーと公開 C# API は維持する。検証は AnimatorControllerDefinitionTest で行う。

IReflector、IScriptSerializable とフィールド宣言の共通処理は Reflection.hpp が所有する。Component・DataAsset・Serializer はこのヘッダーを読み、Script と ScriptProxy を反射のために読み込まない。Script.hpp は既存の利用側との互換性を保つため Reflection.hpp を公開する。衝突情報は CollisionInfo.hpp、粒子の共有列挙型は Components/ParticleTypes.hpp に置き、接地制御やアセット形式から ScriptProxy への依存を作らない。

ScriptProxy の実装は Runtime、Input、Physics、Joint、Audio、Camera、Material、Effects、UI、Scene、Animation、Debug、Rendering、Navigation、Environment、Store の各 Script*Proxies.cpp に分ける。領域固有のヘルパーはその翻訳単位の匿名名前空間へ閉じ、複数領域が使う非所有の解決処理だけを内部 ScriptProxyCommon.hpp に置く。剛体と制御点の共有ヘルパーはそれぞれ ScriptPhysicsProxyHelpers.hpp と ScriptControlPointProxyHelpers.hpp へ置き、その領域の利用側だけが読み込む。

Tween のフレーム待機は内部 CoroutineFrameLoop.cpp の RunFrames に集約する。更新 callback は Coroutine フレームが値で所有し、Tween は開始値・終値・時間軸・経過時間を捕捉して渡す。C++ の中断構文と標準 coroutine ABI は Coroutine.hpp と CoroutineFrameLoop.cpp だけで扱い、プロキシや他の利用側に独自の coroutine 実装を作らない。公開 API とハンドルの所有方式は変更しない。

RunFrames は既存 WaitForFrames(1) を使い、生成フレームを消費しない。Script の UpdateCoroutines、pending キュー、停止要求、障害隔離と破棄が引き続き進行・終了を管理する。所要時間が正なら最初の更新は次フレーム以降、0 以下・NaN なら生成時に終値を適用する。Value は1更新フレームに1回だけ通知し、終了フレームは補間値の代わりに正確な終値を1回通知する。時間が終端を越えても重複通知せず、終了後は通知しない。0 以下・NaN の即時完了も通知は1回とする。Scaled／Unscaled、Shake の固定軸と原点復帰は維持する。検証は TweenCoroutineTest と AgentLintCoroutineBoundaries で行う。

CMake/EngineModuleBoundaries.cmake が各 OBJECT モジュールのソースからリポジトリ内の include を再帰走査し、直接・推移依存と明示リンク先を configure 時に検査する。共通の値型・反射、アセット契約、描画契約、managed POD 契約は実装とは別に分類する。許可表を変えるときはこの設計と検査を同時に更新する。

SceneContracts は Components 以下のデータ契約と、明示した TerrainSplat.hpp、TerrainSurfaceGeometry.hpp、Fields/FlowField.hpp、TomlReflector.hpp に限定する。他の Scene ヘッダーは Scene 実行系として扱い、Assets から Scene.hpp、ISystem、Systems の実行システムや BoneBakeRuntime.hpp を直接・推移的に読み込むことを拒否する。Animator のベイク姿勢・状態型は Components/BoneBakeTypes.hpp に分け、評価関数は BoneBakeRuntime.hpp に残す。走査した既存ソース・ヘッダーの include 変更も構成の再実行対象にする。

| 利用側 | 許可する Engine 内部依存 |
| --- | --- |
| Platform | Platform と API マクロのみ |
| Audio / AI | Platform、共通反射、軽量アセット契約、自領域 |
| Assets | Platform、共通反射、Audio、AI、Scene の Component・値型契約、描画契約 |
| Scene | Platform、Assets、Audio、AI、Managed と各公開契約。Rendering の実装・描画入口への依存は禁止 |
| Rendering | Platform、Assets、Audio、AI、Scene、Managed の公開契約 |
| Managed | Platform、Assets、Audio、AI、Scene、描画の公開契約 |

外部ライブラリもモジュール別に許可し、Platform／Audio／AI から Graphics・Physics・Fluid への依存を拒否する。Editor と Graphics/src 以下の実装ヘッダー（DX バックエンドを含む）は全モジュールで禁止する。公開 Graphics 契約を経由した推移 include も同じ検査で拒否する。構成ルートは Engine 全体を合成する Host として全内部領域への依存を許可し、Editor とバックエンドへの禁止は同じ検査で維持する。Core/Application.hpp と Profiler/ScriptProfiler.hpp も低層 Platform ではなく Host の契約として扱う。EngineModuleBoundaries テストは許可例と禁止される直接・推移・バックエンド・リンクの例を検証する。

Audio／AI が使うパス解決は軽量な AssetPath.hpp から既存 AssetManager へ接続する。ロード用の Model／Material／Graphics 型を利用側へ引き込まない。SynthAsset.hpp の IReflector は前方宣言し、反射の具体定義は実装側で読む。

Sandbox のビルドは実行用コピーを更新する。編集用 SandboxProject は初回だけ不足ファイルを配置し、既存 Assets・meta・設定・起動情報を上書きしない。既存アセットに雛形の GUID を付けない。

SystemScheduler は明示した間接順序も含めて競合を直列化する。Build が循環を検出した場合は false を返し、OnInit と Update を実行しない。正常な依存グラフを完成させてから全システムを初期化する。

ISystem の未宣言アクセスは Unrestricted とし、明示した読み書きがある場合だけ並列化を許す。UIAnimatorSystem は UIImage／UIText に加えて Transform の書込みも宣言する。既存の TransformLateUpdate との順序制約だけに競合回避を依存させない。

UI.IsPointerOverUI は現在の viewport 内ポインターと Canvas の矩形から CPU で判定する。描画・GPU 初期化・直近フレームのイベントフラグに依存せず、Mask、ScrollView、CanvasGroup、明示 UIPointer を反映する。Editor はゲーム viewport 原点からの位置を渡し、Standalone は現在の client size を渡す。クリックや focus は変更しない。WasClicked 等の既存通知は描画フレーム N の結果を Update N+1 で読む契約を維持する。

ホストが使う ResourceManager は Application と SceneManager へ明示登録する。登録は寿命 ID による非所有参照とし、別 Manager の生成・破棄で更新先・フレーム進行・終了処理を切り替えない。SceneManager は Script callback より前に実行 Scene へ同じ参照を設定し、保存読込みも渡された Manager を設定する。Script の材質・動的テクスチャ・Prefab 展開は所属 Scene の Manager を使い、ResourceManager::Active へフォールバックしない。Manager 終了後は未接続として安全に失敗し、別個体へ接続し直さない。Profiler のフレーム開始にもホストが Manager を渡す。ファイル登録した Scene も読込み時点の明示 Manager を使う。Editor の Inspector・AssetBrowser・プリセット・Prefab 復元は既存 EditorContext または Scene の接続を使い、未接続の SceneIO は CPU データだけを復元する。検証は HostResourceBindingTest と ObjectPresetMenuTest で行う。

生成メッシュの GPU 所有者検証・容量確保・転送・二枚スロットの公開は Scene 内部の RuntimeMeshUpload に集約する。ProceduralMesh、Sprite／Line、Cloth は CPU 形状を保持し、Manager 変更・Reset・個別解放を現在のバッファ台帳で検出したら、MeshRenderer の借用を切って両スロットを作成元へ返す。変更のない形状も次の更新で再構築し、Cloth の CPU シミュレーションは GPU の失効だけでは初期化し直さない。新しい頂点・インデックス資源が両方揃ったときだけ描画スロットを公開し、確保失敗時は旧描画スロットと dirty を保って再試行する。別 Manager の旧資源を現在の Manager へ返さない。検証は RuntimeMeshOwnershipTest と既存 PresentationGeometryTest で行う。

PrefabPool の待機列・貸出列は Scene のアドレスではなく寿命世代で識別する。Scene の Clear とデストラクタがその世代の列を退役させ、Scene の移動でも旧世代の列を引き継がない。デストラクタで Script callback は発火しない。Prefab ごとの上限設定は従来どおり ClearAll まで保持する。他 Scene 所有の個体の返却は拒否する。検証は PrefabIdentityTest の Clear・移動・同じアドレス／EntityID の再利用ケースで行う。

組込みアセットの型、Importer、ホットリロード可否は AssetTypes.inl の一つの一覧を正本とする。同じ一覧から初期登録、全解放、失敗キャッシュの消去、再読込みと DLL の明示特殊化を作る。AssetStore<T>::Get 自体も Engine DLL で所有し、Editor・テスト EXE・Engine 内のロード／取得／Importer 登録が同じストアへ接続する。VectorFieldAsset と ParticleCurveAsset もこの境界に含める。カスタム型の汎用テンプレートは組込み型の DLL 境界を代替しない。検証は AssetTypeBoundaryTest で行う。

GPU を所有する Component は設定値と TransientGpuState<State> を分離する。GPU 状態は OwnedResource により作成元へ返し、通常の値コピー・コピー代入は空の GPU 状態、移動は単独所有権の移譲となる。TransientGpuState の自己代入は所有権を維持する。新しい所有フィールドは State へ置き、Scene 複製・Undo 側へ型別ハンドル消去の一覧を追加しない。CPU の保存値・粒子履歴はそれぞれ既存のコピー契約に従い、ParticleRuntime のコピーは CPU 粒子列も初期化して再開始する。二枚の個体専用メッシュも同じ方針でコピーを空にし、再代入と破棄で両スロットを返す。ProceduralMesh の新規作成は従来どおり dirty=None とし、複製・GPU 無効化後は State の OnResetGpuRuntime で全アップロードを要求する。

Scene の終了と Component 削除は既存の Component 登録メタデータを走査し、各型が宣言するリセット境界を呼ぶ。Sprite・Line・ProceduralMesh・Cloth が生成したメッシュへの MeshRenderer の借用参照は、所有メッシュの破棄前に切る。Scene 複製では登録型の runtimeMesh を検出して元個体への借用を切り、生成システムが複製先のメッシュを再公開する。Inspector の値適用も所有メッシュを置き換える前に同じ借用を切る。検証は TransientGpuStateTest と既存の Scene・Inspector・Undo 回帰で行う。

CPU の粒子更新・物理衝突・SubEmitter・スクラブ・ループ検査は Scene の ParticleCpuSimulation.cpp、再生の時刻と要求は ParticlePlayback が所有する。ParticleSimulationSystem は既存の LateUpdate／Always で材質の CPU 設定を解決して一度だけ進める。描画回数、カメラの有無、GPU 初期化の成否で CPU 粒子の時刻を進めない。Rendering は確定した CPU 粒子を写し、GPU 粒子の投入と可視性・予算を扱う。共有の発生形状・GPU 投入値の作成・境界計算は Scene 内部の ParticleSimulationHelpers を使う。画面外 GPU 更新の既存制約は維持する。

UI の CPU 入口は UIInteraction.hpp とし、UpdateUILayout がフォント計測と配置、UpdateUIInteraction が入力・フォーカス・通知を担当する。ホストは Script 更新後、描画資源の準備前に呼ぶ。描画なしのホストも同じ入口へ Viewport・カメラ・ポインターを渡せる。UISystemContext は CPU Context に描画資源を加えたアダプターで、互換呼出しでも CPU 更新は Scene 世代・フレームにつき一度に保つ。Scene View と選択マスクはクリック通知を消さない。深度が使用不能と判明した場合は同フレームでも Scene Depth Canvas の操作だけを取り消す。通知 N → Update N+1 の契約を維持する。

ナビゲーションの候補領域と方向入力のリピート状態、ドラッグの一時状態は CPU Context が所有し、他の Scene と大域共有しない。候補と階層走査のバッファはフレーム間で使い回す。

UI の座標・矩形は内部 UIRuntimeGeometry、リッチテキスト・折返し・省略・自動文字サイズは UITextLayout を正本として CPU と描画で共有する。CPU の FontAtlas は LoadMetrics／PrepareTextMetrics 専用の別キャッシュで、PNG 読込み・SDF ラスタライズ・GPU ページ確保を行わない。BMFont／旧形式の解析と、動的フォントの送り幅・カーニングは描画と同じ実装を使う。GPU 用 Atlas と計測用 Atlas を同じインスタンスで兼用しない。検証は UIInteractionTest、UIQueryTest、ParticlePlaybackTest で行う。

UISystem.cpp と SceneRenderResources.cpp は Rendering、描画を束ねる StandaloneProjectModule.cpp と ProjectRuntime.cpp は Host の合成ルートへ所属させる。ProjectRuntime の公開ヘッダーは UI の描画状態を前方宣言し、所有は実装側へ隠す。SceneUtils／ProjectLauncher の UI 設定適用は CPU Context だけを受け取る。アニメーション診断は SkinningDemandTypes の値型だけを共有し、描画パス実装を読み込まない。水しぶき要求は WaterInteraction が所有する。構成時検査は Scene → Rendering を直接・推移とも禁止し、EngineModuleBoundaries が許可例と禁止例を検証する。

検証は AgentBuild による native のコンパイル・リンク、Scene／Prefab／Component／managed bridge と GPU 所有権の既存回帰、SDK テンプレートの契約を使う。配布 SDK の更新は既存の Preview 入口で行い、実行中の Editor を差し替えない。

## Physics の非所有登録

Collider と RigidBody の正本は従来どおり呼出し側の単独所有とする。Physics 内の RegistrationLifetime は初回登録時に個体ごとの寿命証明だけを生成し、World は弱参照で借用する。実体を共有所有せず、Scene や Engine の型も Physics へ持ち込まない。コピー・移動先へ旧登録を引き継がず、破棄・代入・同じアドレスへの再生成で旧証明を失効させる。

World は BVH の境界照合と filter 呼出しより前に形状・剛体の証明を確認し、失効した登録を全 query から除く。query は登録表を書き換えないため、並行 read と filter 内の再入を維持する。BeginSceneSync／EndSceneSync／Step の所有スレッド上の安全点で失効 slot を退役し、世代を進め、非所有 view と接触 cache・イベント・debug snapshot を破棄または再構築する。制約も対象剛体の証明を保持し、失効した剛体を解く前に退役する。個体の変更・破棄は query と並行に行わない。

Scene／Component の削除、Clear、形状 setter に追加の Physics 呼出しを散らさず、単独所有する実体の終了が登録の失効になる。Scene 切替後も残る World と World 自身の移動・Reset で、旧個体を復活させない。検証は WorldQueryIndexTest の索引あり削除・同一アドレス再生成・コピー・World 移動・剛体と制約の終了、および ManagedPhysicsBridgeTest の Component 除去・形状差替え・Scene Clear 後の4種 query で行う。
