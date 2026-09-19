/// @note FBZZ Engine
/// @note Binding.hlsli | Common
/// @note レジスタ番号の一元定義。全シェーダーがこのファイルを参照する
#ifndef BINDING_HLSLI
#define BINDING_HLSLI

/// @note ---- cbuffer --------------------------------------------------------
#define CB_CAMERA       b0  /// @note CameraConstants   per-frame
#define CB_OBJECT       b1  /// @note ObjectConstants   per-draw
#define CB_MATERIAL     b2  /// @note MaterialConstants per-material
#define CB_LIGHT        b3  /// @note LightConstants    per-frame
#define CB_SHADOW       b4  /// @note ShadowConstants   per-frame
#define CB_POSTPROC     b5  /// @note PostProcConstants per-pass
#define CB_SKINNING     b7  /// @note SkinningConstants per-animated draw
#define CB_ATMOSPHERE   b6  /// @note AtmosphereConstants per-frame (Skydome のみ)

/// @note Velocity パス専用: 前フレームのボーンパレット。
/// @note WHY b2 に相乗りするか: b0〜b13 は全て別用途に埋まっている。Velocity パスは
/// @note 速度しか書かずマテリアルを一切読まないので、このパスに限り b2 が空く。
/// @note Velocity.hlsl / VelocitySkinned.hlsl 以外がこの名前を使ってはいけない。
#define CB_PREV_SKINNING b2

/// @note ---- UI (UISystem / UI マテリアル) -----------------------------------
/// @note UI パスは b0 を CameraConstants ではなく UIConstants として使う。
/// @note WHY 専用スロットへ逃がさないか:
/// @note UI の描画に view / projection は要らない。矩形は Canvas 空間で組み立て、
/// @note ortho 1 本で clip 空間へ送る。カメラ行列を束縛しないパスで b0 を空けておく
/// @note 意味が無く、逆に UI だけ離れた番号を使うと DrawCall の埋め方が UI だけ
/// @note 例外になる。代わりに UI シェーダーは Constants.hlsli を include しない、
/// @note という制約を UICommon.hlsli の #error で機械的に守らせる。
#define CB_UI           b0  /// @note UIConstants per-draw (ortho / color / uvRect / rect)
                            /// @note マテリアルパラメータは CB_MATERIAL (b2) を使う。
                            /// @note 名前も MaterialConstants に揃えること — シェーダー
                            /// @note リフレクションは cbuffer 名で探すため、名前を変えると
                            /// @note .mat の params と Inspector が丸ごと効かなくなる。

/// @note ---- Decal (DecalPass) -----------------------------------------------
/// @note 投影ボリューム・角度フェード・受信レイヤーはエンジンが毎ドロー埋める。
///
/// @note WHY b2 に置かないか:
/// @note シェーダーリフレクションは cbuffer 名 "MaterialConstants" を探す
/// @note (DX11Shader/DX12Shader::BuildDescriptor)。デカール定数をそこへ置くと
/// @note .mat の params を 1 つも束縛できず、デカールだけマテリアルを持てない
/// @note 例外になる。b2 は材質へ明け渡し、エンジン側は空きスロットへ逃がす。
#define CB_DECAL        b10 /// @note DecalConstants per-decal
                            /// @note デカール材質のパラメータは CB_MATERIAL (b2)。
                            /// @note 契約は Material/Decal/DecalCommon.hlsli が持つ。

/// @note ---- Particle (ParticlePass) -----------------------------------------
/// @note ビルボードの展開方法・フリップブック・歪み・煙・自己影といった、エンジンが
/// @note エミッターごとに決める値。材質側からは読み取り専用。
///
/// @note WHY b2 に置かないか:
/// @note CB_DECAL とまったく同じ理由。リフレクションは cbuffer 名 "MaterialConstants" を
/// @note b2 に探すため、ここを占有するとパーティクル材質だけ .mat の [params] を
/// @note 1 つも持てない例外になる。b2 は材質へ明け渡し、エンジン側は空きスロットへ逃がす。
#define CB_PARTICLE     b11 /// @note ParticleRenderConstants per-emitter
                            /// @note パーティクル材質のパラメータは CB_MATERIAL (b2)。
                            /// @note 契約は Material/Effects/ParticleMaterial.hlsli が持つ。

/// @note 残像 1 枚ぶんの色 (古いサンプルほど colorEnd 寄り)。エンジンが毎ドロー埋める。
///
/// @note WHY b2 に置かないか:
/// @note CB_DECAL / CB_PARTICLE とまったく同じ理由。リフレクションは cbuffer 名
/// @note "MaterialConstants" を b2 に探すため、ここを占有すると残像材質だけ .mat の
/// @note [params] を 1 つも持てない例外になる。
/// @note WHY 新しい番号を作らず b6 を借りるか:
/// @note b0〜b13 は既に全て別用途で埋まっている。残像の描画は大気も空も読まないので、
/// @note このパスに限り b6 が空く (CB_PREV_SKINNING が b2 を借りるのと同じ作法)。
/// @note MeshTrail / SkinnedMeshTrail とその材質シェーダー以外がこの名前を使ってはいけない。
#define CB_MESHTRAIL    b6  /// @note MeshTrailConstants per-sample
                            /// @note 残像材質のパラメータは CB_MATERIAL (b2)。
                            /// @note 契約は Material/Effects/MeshTrailMaterial.hlsli が持つ。

/// @note ---- Texture (マテリアル, per-draw) ----------------------------------
#define TEX_ALBEDO          t0
#define TEX_NORMAL          t1
#define TEX_METALLIC_ROUGH  t2  /// @note R=metallic  G=roughness
#define TEX_EMISSIVE        t3
#define TEX_AO              t4

/// @note ---- Texture (パスリソース, per-pass) --------------------------------
#define TEX_GBUFFER0    t5   /// @note albedo(RGB) + roughness(A)
#define TEX_GBUFFER1    t6   /// @note normal(RGB) + metallic(A)
#define TEX_DEPTH       t7
#define TEX_SHADOW      t8
#define TEX_SSAO        t9
#define TEX_BLOOM       t10
#define TEX_ENV_CUBE    t11  /// @note Skybox キューブマップ / IBL
#define TEX_ENV_EQUIRECT t12 /// @note Skydome 等緯度テクスチャ
/// @note カスタムパスが宣言したときだけ束縛される追加入力。IBL / Skydome と時分割で共有する。
/// @note WHY 空きスロットを増やさず時分割か: 全画面のカスタムパスはシーンのジオメトリを
/// @note 描かないので環境マップを読まない。同じフレームでも同時に束縛されることが無い。
#define TEX_CUSTOM_VELOCITY t11 /// @note モーションベクター (RG = 速度)
#define TEX_CUSTOM_NORMAL   t12 /// @note GBuffer 法線 (Deferred 系のみ)
/// @note 各ピクセルの可視サーフェスが属するレイヤー番号 + 1 を格納する。0 = 未描画。
/// @note WHY 0 を「未描画」に使うか: レイヤー 0 (Default) が有効な番号なので、クリア値と
/// @note 区別が付かないと地形やフォリッジのように受信バッファへ描かないジオメトリが
/// @note すべて Default 扱いになる。+1 しておけば「不明なら受信させる」に倒せる。
#define TEX_DECAL_MASK   t13 /// @note デカール受信レイヤーバッファ (レイヤーフィルタ使用時のみ)
/// @note パーティクル自己影の光源側密度バッファ (R=Σα, G=Σα·深度)。
/// @note SSAO と同じ t9 を時分割で使う。パーティクル描画は SSAO を読まないため衝突しない。
#define TEX_PARTICLE_DENSITY t9

/// @note ---- StructuredBuffer (t14〜: テクスチャ SRV と重複しない領域) --------
#define SB_GPU_PARTICLES    t14  /// @note StructuredBuffer<GpuParticle> (VS 描画用 / CS RW 用)
#define SB_GPU_SPAWN        t15  /// @note StructuredBuffer<GpuSpawnEntry> (CS スポーン入力)
/// @note GPU ソート済みの (key, particleIndex)。t15 を CS のスポーン入力と時分割で共有する。
/// @note WHY: スポーン CS と描画 VS は別ステージ・別ディスパッチで、同時にバインドされることがない。
/// @note t16 以降は Advanced Graphics が使い切っているため、空きスロットを増やすより時分割が安い。
#define SB_GPU_SORT         t15  /// @note VS: StructuredBuffer<uint2> (ソート済みインデックス)

/// @note 粒子に効く流れ一式 (内蔵 + シーン)。StructuredBuffer<GpuFlowField>。
/// @note WHY 定数バッファではないか: cbuffer は固定長なので «渡せる力の本数» に上限が要り、
/// @note あふれた分は捨てるしかない。捨てられたのが重力だと粒子がその場に浮くという、
/// @note 設定ミスと区別の付かない壊れ方をする。SRV にすれば上限そのものが消える。
/// @note SB_PUNCTUAL_LIGHTS と同じ t29 を時分割する (粒子 CS はライトを読まない)。
#define SB_PARTICLE_FORCES  t29

/// @note ---- UAV (コンピュートシェーダー出力) ---------------------------------
#define UAV_OUTPUT      u0
#define UAV_OUTPUT2     u1
#define UAV_GPU_PARTICLES   u2   /// @note RWStructuredBuffer<GpuParticle> (CS 書き込み)
/// @note GPU パーティクルのソートキー書き込み先。キー生成 CS では u2 (粒子 SRV) と同時に使うため u3 へ置く。
/// @note UAV_SSR / UAV_CONTACT_SHADOW と同じスロットだが、いずれも逐次パスで同時実行しない。
#define UAV_GPU_SORT        u3   /// @note RWStructuredBuffer<uint2> (key, particleIndex)
/// @note コンピュートスキニングの出力頂点。CS が書き、以降のパスは同じバッファを
/// @note 通常の頂点バッファとして読む (スキニングを 1 フレーム 1 回だけにするため)。
#define UAV_SKINNED_VERTICES u4  /// @note RWStructuredBuffer<SkinnedOutVertex>

/// @note ---- SRV (コンピュートスキニング入力) ---------------------------------
/// @note ComputeCall::srvBuffers[14] / [15] が束縛される (添字 = レジスタ番号)。GPU パーティクルの
/// @note SB_GPU_PARTICLES / SB_GPU_SORT と同じスロットだが、別ディスパッチなので競合しない。
#define SB_SKIN_SRC_VERTICES t14 /// @note StructuredBuffer<SkinSrcVertex>
#define SB_SKIN_BONES        t15 /// @note StructuredBuffer<float4x4> (ボーンパレット)

/// @note ---- Sampler ---------------------------------------------------------
#define SAMPLER_DEFAULT      s0
#define SAMPLER_SHADOW       s1   /// @note SamplerComparisonState (PCF 用)
#define SAMPLER_LINEAR_CLAMP s2   /// @note Linear clamp (IBL BRDF LUT / 3D LUT 用)
#define SAMPLER_POINT_CLAMP  s3   /// @note Point clamp  (TAA 再投影ルックアップ用)
#define SAMPLER_WRAP_LINEAR  s4   /// @note Linear wrap  (ボリューメトリック雲のタイラブル 3D ノイズ用)
/// @note s5〜s8 は DX12PsoCache::MakeStaticSamplers() が静的サンプラーとして常時焼いている。
/// @note 定義がここに無いと「どのフィルタが来るか」がバックエンドのコードを読まないと分からない。
#define SAMPLER_UI           s5   /// @note Linear clamp (UI スプライト / テキスト)
/// @note Cookie アトラス用。設定は SAMPLER_UI と同じ Linear clamp なので同じ枠を使う。
/// @note WHY 同居できるか: UI シェーダーは Constants.hlsli / Lighting.hlsli を include しない
/// @note 規約 (UICommon.hlsli の #error が機械的に守らせている) ため、s5 を宣言する
/// @note シェーダーは「UI か、ライティングを持つマテリアルか」のどちらか一方に必ずなる。
/// @note WHY s2 (SAMPLER_LINEAR_CLAMP) を使わないか: あちらは IBL BRDF LUT 用に
/// @note マテリアルシェーダーが自前の名前で宣言済み。共有ヘッダーから重ねて宣言できない。
#define SAMPLER_COOKIE       s5   /// @note Linear clamp (ライト Cookie)
#define SAMPLER_UI_POINT     s6   /// @note Point  clamp (ドット絵 UI / 拡大時に補間させたくない図版)
/// @note Spot / Point シャドウ用の 2 本目の比較サンプラー。設定は s1 と同一。
/// @note WHY s1 を使い回さないか: s1 の SamplerComparisonState は各マテリアルシェーダーが
/// @note 自前の名前で宣言している (texShadow / sampShadow / gSampShadow …)。
/// @note 共有ヘッダーからもう 1 つ s1 を宣言するとレジスタが二重定義になり、
/// @note かといって呼び出し側の名前は統一されていないので参照もできない。
/// @note 専用スロットを 1 本使うのが、40 以上のシェーダーを書き換えずに済む唯一の道。
#define SAMPLER_SHADOW_PUNCTUAL s7 /// @note SamplerComparisonState (Spot / Point の PCF 用)
/// @note 地形レイヤー (diffuse / normal / aoRoughness) のタイリング用。
/// @note WHY s0 と分けるか: 地形は画面を広く覆い、レイヤーごとに 3 枚を引くので x16 異方性の
/// @note コストが枚数ぶん乗る。x4 なら見た目はほぼ変わらず約 1/3 で済む。
#define SAMPLER_WRAP_ANISO4  s8   /// @note Aniso x4 wrap (地形レイヤー)

/// @note ---- Advanced Graphics (IBL / SSR / TAA / GTAO 等) ----
#define CB_ADVANCED_GRAPHICS b8

#define TEX_IBL_IRRADIANCE   t16  /// @note Diffuse IBL 事前畳み込みキューブマップ
#define TEX_IBL_PREFILTER    t17  /// @note Specular IBL 事前フィルタ済みキューブマップ (roughness → mip)
#define TEX_IBL_BRDF_LUT     t18  /// @note BRDF 積分 LUT (x=scale, y=bias)

#define TEX_SSR              t19  /// @note SSR Compute 出力
#define TEX_VOLUMETRIC       t20  /// @note ボリューメトリックライト Compute 出力
#define TEX_TAA_HISTORY      t21  /// @note TAA 前フレームカラーバッファ
#define TEX_LUT_COLOR_GRADE  t22  /// @note 3D カラーグレーディング LUT
#define TEX_GTAO             t23  /// @note GTAO 結果 (SSAO の代替)
/// @note Forward のマテリアルシェーダーが読む「統一済み画面空間 AO」(GTAO か SSAO のどちらか)。
///
/// @note WHY TEX_SSAO (t9) を使わないか: t9 は Terrain が AO/Roughness レイヤー配列に、
/// @note Water がシャドウマップに、パーティクルが自己影の密度バッファに使っており、
/// @note マテリアル側では空いていない。t23 はマテリアル系のどのシェーダーも宣言していない。
/// @note WHY TEX_GTAO と同じ番号でよいか: t23 を読むのは GTAO のブラー CS (中間出力) と
/// @note Composite (froxel fog) だけで、どちらもマテリアル描画とは別のディスパッチ / パス。
/// @note 同時に束縛されることがない。
#define TEX_SCREEN_AO        t23
#define TEX_CONTACT_SHADOW   t24  /// @note コンタクトシャドウマスク
#define TEX_SCENE_DEPTH      t25  /// @note Forward 不透明物を含む最終シーン深度 (SSR 遮蔽用)
#define TEX_CLOUD_SHAPE      t26  /// @note ボリューメトリック雲 Shape 3D ノイズ (128³ Perlin-Worley)
#define TEX_CLOUD_DETAIL     t27  /// @note ボリューメトリック雲 Detail 3D ノイズ (32³ Worley 高周波)

/// @note 常駐中の 速度場 PNG を積んだ Texture3D アトラス (32³ タイル × N)。
/// @note TEX_CLOUD_SHAPE と t26 を時分割する。**どちらも Texture3D** なので、
/// @note DX12 の null ディスクリプタ次元が食い違う心配が無い組み合わせになっている。
#define TEX_VELOCITY_FIELD   t26

/// @note モーションベクター: RG = 現 UV - 前フレーム UV、B = 書き込み済みフラグ (1)。
/// @note B が 0 の画素 (空・未描画) では読み手が従来どおり深度再投影へフォールバックする。
///
/// @note WHY TEX_CLOUD_SHAPE と時分割か: DrawCall::textures は t0〜t31 の 32 枠で空きが無い。
/// @note 枠を増やすと DX12 のルートシグネチャ (srvRange.NumDescriptors) の変更になり
/// @note 全 PSO へ波及する。雲 3D ノイズを読むのは VolumetricCloud.hlsl だけで、
/// @note TAA / MotionBlur / Velocity は雲を一切参照しないため同時に束縛されない。
/// @note TEX_FROXEL_SCATTER が TEX_VOLUMETRIC と時分割しているのと同じ扱い。
#define TEX_VELOCITY         t26

/// @note ---- Spot / Point シャドウ + Cookie -----------------------------------
/// @note Directional の CSM (TEX_SHADOW) とは別アトラス。
/// @note WHY 相乗りさせないか: CSM は「カメラ視錐台をどう分割するか」でタイル数が決まるのに対し、
/// @note こちらは「シーンにいくつ影付きライトがあるか」で決まる。同じ面積を奪い合わせると、
/// @note ライトを 1 つ置いただけで Directional の遠景カスケードが粗くなる。
#define TEX_PUNCTUAL_SHADOW  t28  /// @note Spot / Point シャドウアトラス (深度)
/// @note スポット / 点光源へ被せる投影テクスチャ (Unity の Cookie / UE の Light Function)。
/// @note 単一アトラスに複数 Cookie を敷き詰め、ライトごとに矩形で切り出す。
#define TEX_LIGHT_COOKIE     t31  /// @note Cookie アトラス (RGBA)

#define UAV_SSR              u3   /// @note SSR Compute 書き込み先
#define UAV_VOLUMETRIC       u4   /// @note ボリューメトリック Compute 書き込み先
#define UAV_MOTION_BLUR      u5   /// @note モーションブラー Compute 書き込み先
#define UAV_GTAO_RAW         u6   /// @note GTAO RAW Compute 書き込み先
#define UAV_GTAO_BLUR        u7   /// @note GTAO ブラー後 Compute 書き込み先
/// @note WHY: DX11 SM5.0 の CS UAV スロットは u0〜u7 の 8 本しかない。
/// @note 下記 2 パスはどちらも他の CS と同時実行しない逐次パスなので、
/// @note 既存スロットを再利用することで上限内に収める。
#define UAV_CONTACT_SHADOW   u3   /// @note コンタクトシャドウ Compute 書き込み先 (UAV_SSR と時分割で再利用)
#define UAV_BRDF_LUT         u0   /// @note BRDF LUT ベイク出力 (起動時 1 回のみ。UAV_OUTPUT と時分割で再利用)

/// @note ---- フロクセル ボリューメトリック フォグ ------------------------------
/// @note 視錐台を XY タイル × 対数 Z スライスへ切ったボリューム。2 パスで作る。
/// @note 1. Inject    : 各フロクセルの散乱色と消散係数を求めて gFroxelScatter へ書く
/// @note 2. Integrate : Z 方向へ前から積分し、透過率込みの結果を gFroxelIntegrated へ書く
/// @note 適用は Composite が深度からスライスを引いて 1 回サンプルするだけ。
///
/// @note WHY u0/u1 を使い回すか: どちらのパスも他の CS と同時実行しない逐次パスで、
/// @note DX11 SM5.0 の CS UAV は u0〜u7 しかない。専用スロットを増やすより時分割が安い。
#define UAV_FROXEL_SCATTER    u0  /// @note RWTexture3D<float4> (UAV_OUTPUT と時分割)
#define UAV_FROXEL_INTEGRATED u1  /// @note RWTexture3D<float4> (UAV_OUTPUT2 と時分割)

/// @note Bloom アップサンプルが「同じ寸法のダウンサンプル結果」を足すための入力。
/// @note WHY 書き込み先の UAV から読まないか: コンピュート用テクスチャは RGBA16F で、
/// @note D3D11 の typed UAV load が保証されるのは R32 系だけ。RWTexture2D<float4> を
/// @note 読むと未定義動作になり、実際に画面が 2x2 に割れる形で壊れた。
/// @note 読むものは必ず SRV から読む。t9 (TEX_SSAO) は Bloom の経路では使われない。
#define TEX_BLOOM_ADD         t9

#define TEX_FROXEL_SCATTER    t20 /// @note Integrate の入力 (TEX_VOLUMETRIC と時分割: 同時に読まない)
#define TEX_FROXEL_FOG        t23 /// @note Composite が読む積分済みボリューム (TEX_GTAO と時分割)

/// @note Inject が読む前フレームの散乱ボリューム。t20 を TEX_FROXEL_SCATTER と時分割する。
/// @note WHY 同じ番号でよいか: Inject と Integrate は別のディスパッチで、どちらも
/// @note 「そのパスが読むフロクセルボリューム」を t20 に置く。同時には読まない。
/// @note サンプラーは 3D の linear clamp が要るだけなので s2 を共用する (s5/s7 は満杯)。
#define TEX_FROXEL_HISTORY     t20
#define SAMPLER_FROXEL_HISTORY SAMPLER_LINEAR_CLAMP

/// @note フロクセル霧の定数。
#define CB_FROXEL_FOG        b13 /// @note FroxelFogConstants

/// @note bindless ディスクリプタ添字ブロック。全シェーダーが同じ並びで受け取る。
/// @note 詳細は Common/BindlessIndices.hlsli と Docs/design/bindless.md
#define CB_BINDLESS_INDICES  b14

/// @note ---- Clustered Lighting (Forward+ / Deferred+) ------------------------
#define CB_CLUSTER           b9   /// @note ClusterConstants (グリッド寸法 / Z スライス係数 / ライト数)

/// @note ---- Spot / Point シャドウ + Cookie の定数 ----------------------------
/// @note WHY b4 (ShadowConstants) へ足さないか: あちらは 464 bytes 固定で C++ 側に
/// @note static_assert があり、Terrain / Water / パーティクルまで含む 20 以上の
/// @note シェーダーが同じレイアウトを読む。1 つ追加するたびに全経路の再検証が要る。
/// @note Spot / Point は「読むシェーダーが増えていく途中」なので、別 cbuffer にして
/// @note 束縛していないパスが 0 埋め (= 影なし) で素通りできるようにする。
#define CB_PUNCTUAL_SHADOW   b12  /// @note PunctualShadowConstants (VP / アトラス矩形 / バイアス / Cookie)

/// @note ライトデータ。DrawCall::psBuffers[0..1] と ComputeCall::srvBuffers[29] / [30] が束縛される。
/// @note LAYOUT: Engine/Renderer/DrawCall.hpp の kPsBufferBaseSlot (=29) と一致させること。
/// @note WHY t14/t15 を使わないか: あちらは両バックエンドとも頂点シェーダー専用スロットで、
/// @note PS からは読めない (DX11 は VSSetShaderResources、DX12 の root param 15 は VERTEX 可視)。
/// @note NOTE: フロクセル霧の Inject CS もここを読む。CS の SRV テーブルは t0〜t31 を覆うので、
/// @note PS と同じレジスタのまま ComputeCall::srvBuffers 経由で渡せる。
#define SB_PUNCTUAL_LIGHTS   t29  /// @note StructuredBuffer<PunctualLight>  点光源+スポットを統合した配列
#define SB_CLUSTER_INDICES   t30  /// @note StructuredBuffer<uint>  [cluster*stride] = 個数, 続けてライト番号

/// @note カリング CS の入力。ComputeCall::srvBuffers[14] が束縛される。
/// @note GPU パーティクル / スキニングと同じスロットだが、別ディスパッチなので競合しない。
#define SB_CLUSTER_LIGHTS_CS t14  /// @note StructuredBuffer<PunctualLight>

/// @note カリング CS の出力。ComputeCall::uavBuffers[0] が u2 へ束縛される。
/// @note WHY UAV が 1 本で足りるか: 1 スレッド = 1 クラスタにするとカウンタがスレッド内で完結するため、
/// @note InterlockedAdd も offset テーブルも不要になり、固定ストライドで直接書ける。
/// @note おかげで UAV_SSR / UAV_CONTACT_SHADOW / UAV_GPU_SORT が密集する u3 を避けられる。
#define UAV_CLUSTER_LIGHTS   u2   /// @note RWStructuredBuffer<uint> (UAV_GPU_PARTICLES と時分割で再利用)


/// @note ---- bindless 用の数値スロット ------------------------------------------
/// @note 上の t0〜t31 と 1:1 の数値版。宣言マクロ (FBZZ_TEX2D 等) の引数はレジスタ名では
/// @note なく数値でなければならないため、同じ割り当てを数値でも持つ。
///
/// @note WARNING: 上の定義を動かしたらこちらも同じ値へ直すこと。食い違っても «コンパイルは
/// @note 通り、別のテクスチャが貼られる» としてしか現れない。
/// @note @see Docs/design/bindless.md
#define TEX_ALBEDO_SLOT              0
#define TEX_NORMAL_SLOT              1
#define TEX_METALLIC_ROUGH_SLOT      2
#define TEX_EMISSIVE_SLOT            3
#define TEX_AO_SLOT                  4
#define TEX_GBUFFER0_SLOT            5
#define TEX_GBUFFER1_SLOT            6
#define TEX_DEPTH_SLOT               7
#define TEX_SHADOW_SLOT              8
#define TEX_BLOOM_ADD_SLOT           9
#define TEX_PARTICLE_DENSITY_SLOT    9
#define TEX_SSAO_SLOT                9
#define TEX_BLOOM_SLOT               10
#define TEX_CUSTOM_VELOCITY_SLOT     11
#define TEX_ENV_CUBE_SLOT            11
#define TEX_CUSTOM_NORMAL_SLOT       12
#define TEX_ENV_EQUIRECT_SLOT        12
#define TEX_DECAL_MASK_SLOT          13
#define SB_CLUSTER_LIGHTS_CS_SLOT    14
#define SB_GPU_PARTICLES_SLOT        14
#define SB_SKIN_SRC_VERTICES_SLOT    14
#define SB_GPU_SORT_SLOT             15
#define SB_GPU_SPAWN_SLOT            15
#define SB_SKIN_BONES_SLOT           15
#define TEX_IBL_IRRADIANCE_SLOT      16
#define TEX_IBL_PREFILTER_SLOT       17
#define TEX_IBL_BRDF_LUT_SLOT        18
#define TEX_SSR_SLOT                 19
#define TEX_FROXEL_HISTORY_SLOT      20
#define TEX_FROXEL_SCATTER_SLOT      20
#define TEX_VOLUMETRIC_SLOT          20
#define TEX_TAA_HISTORY_SLOT         21
#define TEX_LUT_COLOR_GRADE_SLOT     22
#define TEX_FROXEL_FOG_SLOT          23
#define TEX_GTAO_SLOT                23
#define TEX_SCREEN_AO_SLOT           23
#define TEX_CONTACT_SHADOW_SLOT      24
#define TEX_SCENE_DEPTH_SLOT         25
#define TEX_CLOUD_SHAPE_SLOT         26
#define TEX_VELOCITY_SLOT            26
#define TEX_VELOCITY_FIELD_SLOT      26
#define TEX_CLOUD_DETAIL_SLOT        27
#define TEX_PUNCTUAL_SHADOW_SLOT     28
#define SB_PARTICLE_FORCES_SLOT      29
#define SB_PUNCTUAL_LIGHTS_SLOT      29
#define SB_CLUSTER_INDICES_SLOT      30
#define TEX_LIGHT_COOKIE_SLOT        31

/// @note 頂点シェーダー側の添字空間 (旧 VS 用 SRV テーブル)。
#define VS_SB_INSTANCE_SLOT          0
#define VS_SB_BUFFER0_SLOT           1
#define VS_SB_BUFFER1_SLOT           2

/// @note ---- bindless 用の UAV 数値スロット --------------------------------------
/// @note 上の u0〜u7 と 1:1 の数値版。FBZZ_RWTEX* / FBZZ_RWSBUFFER の引数に使う。
/// @note WARNING: 上の定義を動かしたらこちらも直すこと (食い違うと別の UAV へ書き込む)。
#define UAV_BRDF_LUT_SLOT            0
#define UAV_FROXEL_SCATTER_SLOT      0
#define UAV_OUTPUT_SLOT              0
#define UAV_FROXEL_INTEGRATED_SLOT   1
#define UAV_OUTPUT2_SLOT             1
#define UAV_CLUSTER_LIGHTS_SLOT      2
#define UAV_GPU_PARTICLES_SLOT       2
#define UAV_CONTACT_SHADOW_SLOT      3
#define UAV_GPU_SORT_SLOT            3
#define UAV_SSR_SLOT                 3
#define UAV_SKINNED_VERTICES_SLOT    4
#define UAV_VOLUMETRIC_SLOT          4
#define UAV_MOTION_BLUR_SLOT         5
#define UAV_GTAO_RAW_SLOT            6
#define UAV_GTAO_BLUR_SLOT           7
#endif /// @note BINDING_HLSLI
