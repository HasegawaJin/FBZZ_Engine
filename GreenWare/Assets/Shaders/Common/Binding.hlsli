// FBZZ Engine
// Binding.hlsli | Common
// レジスタ番号の一元定義。全シェーダーがこのファイルを参照する
#ifndef BINDING_HLSLI
#define BINDING_HLSLI

// ---- cbuffer --------------------------------------------------------
#define CB_CAMERA       b0  // CameraConstants   per-frame
#define CB_OBJECT       b1  // ObjectConstants   per-draw
#define CB_MATERIAL     b2  // MaterialConstants per-material
#define CB_LIGHT        b3  // LightConstants    per-frame
#define CB_SHADOW       b4  // ShadowConstants   per-frame
#define CB_POSTPROC     b5  // PostProcConstants per-pass
#define CB_SKINNING     b7  // SkinningConstants per-animated draw
#define CB_ATMOSPHERE   b6  // AtmosphereConstants per-frame (Skydome のみ)

// Velocity パス専用: 前フレームのボーンパレット。
// WHY b2 に相乗りするか: b0〜b13 は全て別用途に埋まっている。Velocity パスは
//     速度しか書かずマテリアルを一切読まないので、このパスに限り b2 が空く。
//     Velocity.hlsl / VelocitySkinned.hlsl 以外がこの名前を使ってはいけない。
#define CB_PREV_SKINNING b2

// ---- UI (UISystem / UI マテリアル) -----------------------------------
// UI パスは b0 を CameraConstants ではなく UIConstants として使う。
// WHY 専用スロットへ逃がさないか:
//   UI の描画に view / projection は要らない。矩形は Canvas 空間で組み立て、
//   ortho 1 本で clip 空間へ送る。カメラ行列を束縛しないパスで b0 を空けておく
//   意味が無く、逆に UI だけ離れた番号を使うと DrawCall の埋め方が UI だけ
//   例外になる。代わりに UI シェーダーは Constants.hlsli を include しない、
//   という制約を UICommon.hlsli の #error で機械的に守らせる。
#define CB_UI           b0  // UIConstants per-draw (ortho / color / uvRect / rect)
                            // マテリアルパラメータは CB_MATERIAL (b2) を使う。
                            // 名前も MaterialConstants に揃えること — シェーダー
                            // リフレクションは cbuffer 名で探すため、名前を変えると
                            // .mat の params と Inspector が丸ごと効かなくなる。

// ---- Decal (DecalPass) -----------------------------------------------
// 投影ボリューム・角度フェード・受信レイヤーはエンジンが毎ドロー埋める。
//
// WHY b2 に置かないか:
//   シェーダーリフレクションは cbuffer 名 "MaterialConstants" を探す
//   (DX11Shader/DX12Shader::BuildDescriptor)。デカール定数をそこへ置くと
//   .mat の params を 1 つも束縛できず、デカールだけマテリアルを持てない
//   例外になる。b2 は材質へ明け渡し、エンジン側は空きスロットへ逃がす。
#define CB_DECAL        b10 // DecalConstants per-decal
                            // デカール材質のパラメータは CB_MATERIAL (b2)。
                            // 契約は Material/Decal/DecalCommon.hlsli が持つ。

// ---- Particle (ParticlePass) -----------------------------------------
// ビルボードの展開方法・フリップブック・歪み・煙・自己影といった、エンジンが
// エミッターごとに決める値。材質側からは読み取り専用。
//
// WHY b2 に置かないか:
//   CB_DECAL とまったく同じ理由。リフレクションは cbuffer 名 "MaterialConstants" を
//   b2 に探すため、ここを占有するとパーティクル材質だけ .mat の [params] を
//   1 つも持てない例外になる。b2 は材質へ明け渡し、エンジン側は空きスロットへ逃がす。
#define CB_PARTICLE     b11 // ParticleRenderConstants per-emitter
                            // パーティクル材質のパラメータは CB_MATERIAL (b2)。
                            // 契約は Material/Effects/ParticleMaterial.hlsli が持つ。

// 残像 1 枚ぶんの色 (古いサンプルほど colorEnd 寄り)。エンジンが毎ドロー埋める。
//
// WHY b2 に置かないか:
//   CB_DECAL / CB_PARTICLE とまったく同じ理由。リフレクションは cbuffer 名
//   "MaterialConstants" を b2 に探すため、ここを占有すると残像材質だけ .mat の
//   [params] を 1 つも持てない例外になる。
// WHY 新しい番号を作らず b6 を借りるか:
//   b0〜b13 は既に全て別用途で埋まっている。残像の描画は大気も空も読まないので、
//   このパスに限り b6 が空く (CB_PREV_SKINNING が b2 を借りるのと同じ作法)。
//   MeshTrail / SkinnedMeshTrail とその材質シェーダー以外がこの名前を使ってはいけない。
#define CB_MESHTRAIL    b6  // MeshTrailConstants per-sample
                            // 残像材質のパラメータは CB_MATERIAL (b2)。
                            // 契約は Material/Effects/MeshTrailMaterial.hlsli が持つ。

// ---- Texture (マテリアル, per-draw) ----------------------------------
#define TEX_ALBEDO          t0
#define TEX_NORMAL          t1
#define TEX_METALLIC_ROUGH  t2  // R=metallic  G=roughness
#define TEX_EMISSIVE        t3
#define TEX_AO              t4

// ---- Texture (パスリソース, per-pass) --------------------------------
#define TEX_GBUFFER0    t5   // albedo(RGB) + roughness(A)
#define TEX_GBUFFER1    t6   // normal(RGB) + metallic(A)
#define TEX_DEPTH       t7
#define TEX_SHADOW      t8
#define TEX_SSAO        t9
#define TEX_BLOOM       t10
#define TEX_ENV_CUBE    t11  // Skybox キューブマップ / IBL
#define TEX_ENV_EQUIRECT t12 // Skydome 等緯度テクスチャ
// カスタムパスが宣言したときだけ束縛される追加入力。IBL / Skydome と時分割で共有する。
// WHY 空きスロットを増やさず時分割か: 全画面のカスタムパスはシーンのジオメトリを
//     描かないので環境マップを読まない。同じフレームでも同時に束縛されることが無い。
#define TEX_CUSTOM_VELOCITY t11 // モーションベクター (RG = 速度)
#define TEX_CUSTOM_NORMAL   t12 // GBuffer 法線 (Deferred 系のみ)
// 各ピクセルの可視サーフェスが属するレイヤー番号 + 1 を格納する。0 = 未描画。
// WHY 0 を「未描画」に使うか: レイヤー 0 (Default) が有効な番号なので、クリア値と
//     区別が付かないと地形やフォリッジのように受信バッファへ描かないジオメトリが
//     すべて Default 扱いになる。+1 しておけば「不明なら受信させる」に倒せる。
#define TEX_DECAL_MASK   t13 // デカール受信レイヤーバッファ (レイヤーフィルタ使用時のみ)
// パーティクル自己影の光源側密度バッファ (R=Σα, G=Σα·深度)。
// SSAO と同じ t9 を時分割で使う。パーティクル描画は SSAO を読まないため衝突しない。
#define TEX_PARTICLE_DENSITY t9

// ---- StructuredBuffer (t14〜: テクスチャ SRV と重複しない領域) --------
#define SB_GPU_PARTICLES    t14  // StructuredBuffer<GpuParticle> (VS 描画用 / CS RW 用)
#define SB_GPU_SPAWN        t15  // StructuredBuffer<GpuSpawnEntry> (CS スポーン入力)
// GPU ソート済みの (key, particleIndex)。t15 を CS のスポーン入力と時分割で共有する。
// WHY: スポーン CS と描画 VS は別ステージ・別ディスパッチで、同時にバインドされることがない。
//      t16 以降は Advanced Graphics が使い切っているため、空きスロットを増やすより時分割が安い。
#define SB_GPU_SORT         t15  // VS: StructuredBuffer<uint2> (ソート済みインデックス)

// ---- UAV (コンピュートシェーダー出力) ---------------------------------
#define UAV_OUTPUT      u0
#define UAV_OUTPUT2     u1
#define UAV_GPU_PARTICLES   u2   // RWStructuredBuffer<GpuParticle> (CS 書き込み)
// GPU パーティクルのソートキー書き込み先。キー生成 CS では u2 (粒子 SRV) と同時に使うため u3 へ置く。
// UAV_SSR / UAV_CONTACT_SHADOW と同じスロットだが、いずれも逐次パスで同時実行しない。
#define UAV_GPU_SORT        u3   // RWStructuredBuffer<uint2> (key, particleIndex)
// コンピュートスキニングの出力頂点。CS が書き、以降のパスは同じバッファを
// 通常の頂点バッファとして読む (スキニングを 1 フレーム 1 回だけにするため)。
#define UAV_SKINNED_VERTICES u4  // RWStructuredBuffer<SkinnedOutVertex>

// ---- SRV (コンピュートスキニング入力) ---------------------------------
// ComputeCall::srvBuffers[14] / [15] が束縛される (添字 = レジスタ番号)。GPU パーティクルの
// SB_GPU_PARTICLES / SB_GPU_SORT と同じスロットだが、別ディスパッチなので競合しない。
#define SB_SKIN_SRC_VERTICES t14 // StructuredBuffer<SkinSrcVertex>
#define SB_SKIN_BONES        t15 // StructuredBuffer<float4x4> (ボーンパレット)

// ---- Sampler ---------------------------------------------------------
#define SAMPLER_DEFAULT      s0
#define SAMPLER_SHADOW       s1   // SamplerComparisonState (PCF 用)
#define SAMPLER_LINEAR_CLAMP s2   // Linear clamp (IBL BRDF LUT / 3D LUT 用)
#define SAMPLER_POINT_CLAMP  s3   // Point clamp  (TAA 再投影ルックアップ用)
#define SAMPLER_WRAP_LINEAR  s4   // Linear wrap  (ボリューメトリック雲のタイラブル 3D ノイズ用)
// s5〜s8 は DX12PsoCache::MakeStaticSamplers() が静的サンプラーとして常時焼いている。
// 定義がここに無いと「どのフィルタが来るか」がバックエンドのコードを読まないと分からない。
#define SAMPLER_UI           s5   // Linear clamp (UI スプライト / テキスト)
// Cookie アトラス用。設定は SAMPLER_UI と同じ Linear clamp なので同じ枠を使う。
// WHY 同居できるか: UI シェーダーは Constants.hlsli / Lighting.hlsli を include しない
//     規約 (UICommon.hlsli の #error が機械的に守らせている) ため、s5 を宣言する
//     シェーダーは「UI か、ライティングを持つマテリアルか」のどちらか一方に必ずなる。
// WHY s2 (SAMPLER_LINEAR_CLAMP) を使わないか: あちらは IBL BRDF LUT 用に
//     マテリアルシェーダーが自前の名前で宣言済み。共有ヘッダーから重ねて宣言できない。
#define SAMPLER_COOKIE       s5   // Linear clamp (ライト Cookie)
#define SAMPLER_UI_POINT     s6   // Point  clamp (ドット絵 UI / 拡大時に補間させたくない図版)
// Spot / Point シャドウ用の 2 本目の比較サンプラー。設定は s1 と同一。
// WHY s1 を使い回さないか: s1 の SamplerComparisonState は各マテリアルシェーダーが
//     自前の名前で宣言している (texShadow / sampShadow / gSampShadow …)。
//     共有ヘッダーからもう 1 つ s1 を宣言するとレジスタが二重定義になり、
//     かといって呼び出し側の名前は統一されていないので参照もできない。
//     専用スロットを 1 本使うのが、40 以上のシェーダーを書き換えずに済む唯一の道。
#define SAMPLER_SHADOW_PUNCTUAL s7 // SamplerComparisonState (Spot / Point の PCF 用)
// 地形レイヤー (diffuse / normal / aoRoughness) のタイリング用。
// WHY s0 と分けるか: 地形は画面を広く覆い、レイヤーごとに 3 枚を引くので x16 異方性の
//     コストが枚数ぶん乗る。x4 なら見た目はほぼ変わらず約 1/3 で済む。
#define SAMPLER_WRAP_ANISO4  s8   // Aniso x4 wrap (地形レイヤー)

// ---- Advanced Graphics (IBL / SSR / TAA / GTAO 等) ----
#define CB_ADVANCED_GRAPHICS b8

#define TEX_IBL_IRRADIANCE   t16  // Diffuse IBL 事前畳み込みキューブマップ
#define TEX_IBL_PREFILTER    t17  // Specular IBL 事前フィルタ済みキューブマップ (roughness → mip)
#define TEX_IBL_BRDF_LUT     t18  // BRDF 積分 LUT (x=scale, y=bias)

#define TEX_SSR              t19  // SSR Compute 出力
#define TEX_VOLUMETRIC       t20  // ボリューメトリックライト Compute 出力
#define TEX_TAA_HISTORY      t21  // TAA 前フレームカラーバッファ
#define TEX_LUT_COLOR_GRADE  t22  // 3D カラーグレーディング LUT
#define TEX_GTAO             t23  // GTAO 結果 (SSAO の代替)
// Forward のマテリアルシェーダーが読む「統一済み画面空間 AO」(GTAO か SSAO のどちらか)。
//
// WHY TEX_SSAO (t9) を使わないか: t9 は Terrain が AO/Roughness レイヤー配列に、
//     Water がシャドウマップに、パーティクルが自己影の密度バッファに使っており、
//     マテリアル側では空いていない。t23 はマテリアル系のどのシェーダーも宣言していない。
// WHY TEX_GTAO と同じ番号でよいか: t23 を読むのは GTAO のブラー CS (中間出力) と
//     Composite (froxel fog) だけで、どちらもマテリアル描画とは別のディスパッチ / パス。
//     同時に束縛されることがない。
#define TEX_SCREEN_AO        t23
#define TEX_CONTACT_SHADOW   t24  // コンタクトシャドウマスク
#define TEX_SCENE_DEPTH      t25  // Forward 不透明物を含む最終シーン深度 (SSR 遮蔽用)
#define TEX_CLOUD_SHAPE      t26  // ボリューメトリック雲 Shape 3D ノイズ (128³ Perlin-Worley)
#define TEX_CLOUD_DETAIL     t27  // ボリューメトリック雲 Detail 3D ノイズ (32³ Worley 高周波)

// モーションベクター: RG = 現 UV - 前フレーム UV、B = 書き込み済みフラグ (1)。
// B が 0 の画素 (空・未描画) では読み手が従来どおり深度再投影へフォールバックする。
//
// WHY TEX_CLOUD_SHAPE と時分割か: DrawCall::textures は t0〜t31 の 32 枠で空きが無い。
//     枠を増やすと DX12 のルートシグネチャ (srvRange.NumDescriptors) の変更になり
//     全 PSO へ波及する。雲 3D ノイズを読むのは VolumetricCloud.hlsl だけで、
//     TAA / MotionBlur / Velocity は雲を一切参照しないため同時に束縛されない。
//     TEX_FROXEL_SCATTER が TEX_VOLUMETRIC と時分割しているのと同じ扱い。
#define TEX_VELOCITY         t26

// ---- Spot / Point シャドウ + Cookie -----------------------------------
// Directional の CSM (TEX_SHADOW) とは別アトラス。
// WHY 相乗りさせないか: CSM は「カメラ視錐台をどう分割するか」でタイル数が決まるのに対し、
//     こちらは「シーンにいくつ影付きライトがあるか」で決まる。同じ面積を奪い合わせると、
//     ライトを 1 つ置いただけで Directional の遠景カスケードが粗くなる。
#define TEX_PUNCTUAL_SHADOW  t28  // Spot / Point シャドウアトラス (深度)
// スポット / 点光源へ被せる投影テクスチャ (Unity の Cookie / UE の Light Function)。
// 単一アトラスに複数 Cookie を敷き詰め、ライトごとに矩形で切り出す。
#define TEX_LIGHT_COOKIE     t31  // Cookie アトラス (RGBA)

#define UAV_SSR              u3   // SSR Compute 書き込み先
#define UAV_VOLUMETRIC       u4   // ボリューメトリック Compute 書き込み先
#define UAV_MOTION_BLUR      u5   // モーションブラー Compute 書き込み先
#define UAV_GTAO_RAW         u6   // GTAO RAW Compute 書き込み先
#define UAV_GTAO_BLUR        u7   // GTAO ブラー後 Compute 書き込み先
// WHY: DX11 SM5.0 の CS UAV スロットは u0〜u7 の 8 本しかない。
//      下記 2 パスはどちらも他の CS と同時実行しない逐次パスなので、
//      既存スロットを再利用することで上限内に収める。
#define UAV_CONTACT_SHADOW   u3   // コンタクトシャドウ Compute 書き込み先 (UAV_SSR と時分割で再利用)
#define UAV_BRDF_LUT         u0   // BRDF LUT ベイク出力 (起動時 1 回のみ。UAV_OUTPUT と時分割で再利用)

// ---- フロクセル ボリューメトリック フォグ ------------------------------
// 視錐台を XY タイル × 対数 Z スライスへ切ったボリューム。2 パスで作る。
//   1. Inject    : 各フロクセルの散乱色と消散係数を求めて gFroxelScatter へ書く
//   2. Integrate : Z 方向へ前から積分し、透過率込みの結果を gFroxelIntegrated へ書く
// 適用は Composite が深度からスライスを引いて 1 回サンプルするだけ。
//
// WHY u0/u1 を使い回すか: どちらのパスも他の CS と同時実行しない逐次パスで、
//     DX11 SM5.0 の CS UAV は u0〜u7 しかない。専用スロットを増やすより時分割が安い。
#define UAV_FROXEL_SCATTER    u0  // RWTexture3D<float4> (UAV_OUTPUT と時分割)
#define UAV_FROXEL_INTEGRATED u1  // RWTexture3D<float4> (UAV_OUTPUT2 と時分割)

// Bloom アップサンプルが「同じ寸法のダウンサンプル結果」を足すための入力。
// WHY 書き込み先の UAV から読まないか: コンピュート用テクスチャは RGBA16F で、
//     D3D11 の typed UAV load が保証されるのは R32 系だけ。RWTexture2D<float4> を
//     読むと未定義動作になり、実際に画面が 2x2 に割れる形で壊れた。
//     読むものは必ず SRV から読む。t9 (TEX_SSAO) は Bloom の経路では使われない。
#define TEX_BLOOM_ADD         t9

#define TEX_FROXEL_SCATTER    t20 // Integrate の入力 (TEX_VOLUMETRIC と時分割: 同時に読まない)
#define TEX_FROXEL_FOG        t23 // Composite が読む積分済みボリューム (TEX_GTAO と時分割)

// Inject が読む前フレームの散乱ボリューム。t20 を TEX_FROXEL_SCATTER と時分割する。
// WHY 同じ番号でよいか: Inject と Integrate は別のディスパッチで、どちらも
//     「そのパスが読むフロクセルボリューム」を t20 に置く。同時には読まない。
//     サンプラーは 3D の linear clamp が要るだけなので s2 を共用する (s5/s7 は満杯)。
#define TEX_FROXEL_HISTORY     t20
#define SAMPLER_FROXEL_HISTORY SAMPLER_LINEAR_CLAMP

// フロクセル霧の定数。
#define CB_FROXEL_FOG        b13 // FroxelFogConstants

// ---- Clustered Lighting (Forward+ / Deferred+) ------------------------
#define CB_CLUSTER           b9   // ClusterConstants (グリッド寸法 / Z スライス係数 / ライト数)

// ---- Spot / Point シャドウ + Cookie の定数 ----------------------------
// WHY b4 (ShadowConstants) へ足さないか: あちらは 464 bytes 固定で C++ 側に
//     static_assert があり、Terrain / Water / パーティクルまで含む 20 以上の
//     シェーダーが同じレイアウトを読む。1 つ追加するたびに全経路の再検証が要る。
//     Spot / Point は「読むシェーダーが増えていく途中」なので、別 cbuffer にして
//     束縛していないパスが 0 埋め (= 影なし) で素通りできるようにする。
#define CB_PUNCTUAL_SHADOW   b12  // PunctualShadowConstants (VP / アトラス矩形 / バイアス / Cookie)

// ライトデータ。DrawCall::psBuffers[0..1] と ComputeCall::srvBuffers[29] / [30] が束縛される。
// LAYOUT: Engine/Renderer/DrawCall.hpp の kPsBufferBaseSlot (=29) と一致させること。
// WHY t14/t15 を使わないか: あちらは両バックエンドとも頂点シェーダー専用スロットで、
//     PS からは読めない (DX11 は VSSetShaderResources、DX12 の root param 15 は VERTEX 可視)。
// NOTE: フロクセル霧の Inject CS もここを読む。CS の SRV テーブルは t0〜t31 を覆うので、
//       PS と同じレジスタのまま ComputeCall::srvBuffers 経由で渡せる。
#define SB_PUNCTUAL_LIGHTS   t29  // StructuredBuffer<PunctualLight>  点光源+スポットを統合した配列
#define SB_CLUSTER_INDICES   t30  // StructuredBuffer<uint>  [cluster*stride] = 個数, 続けてライト番号

// カリング CS の入力。ComputeCall::srvBuffers[14] が束縛される。
// GPU パーティクル / スキニングと同じスロットだが、別ディスパッチなので競合しない。
#define SB_CLUSTER_LIGHTS_CS t14  // StructuredBuffer<PunctualLight>

// カリング CS の出力。ComputeCall::uavBuffers[0] が u2 へ束縛される。
// WHY UAV が 1 本で足りるか: 1 スレッド = 1 クラスタにするとカウンタがスレッド内で完結するため、
//     InterlockedAdd も offset テーブルも不要になり、固定ストライドで直接書ける。
//     おかげで UAV_SSR / UAV_CONTACT_SHADOW / UAV_GPU_SORT が密集する u3 を避けられる。
#define UAV_CLUSTER_LIGHTS   u2   // RWStructuredBuffer<uint> (UAV_GPU_PARTICLES と時分割で再利用)

#endif // BINDING_HLSLI
