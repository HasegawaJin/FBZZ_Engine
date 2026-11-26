/// @file    Binding.hlsli
/// @brief   描画・Compute のスロット契約。C++ の ShaderBindingSlots.hpp もこの定義から生成する。
/// @author  Hasegawa Jin
/// @date    2026-05-19
#ifndef BINDING_HLSLI
#define BINDING_HLSLI

#define FBZZ_REGISTER_TOKEN(prefix, slot) prefix##slot
#define FBZZ_REGISTER(prefix, slot) FBZZ_REGISTER_TOKEN(prefix, slot)

/// @note ---- cbuffer --------------------------------------------------------
#define CB_CAMERA       FBZZ_REGISTER(b, CB_CAMERA_SLOT)  /// @note CameraConstants   per-frame
#define CB_OBJECT       FBZZ_REGISTER(b, CB_OBJECT_SLOT)  /// @note ObjectConstants   per-draw
#define CB_MATERIAL     FBZZ_REGISTER(b, CB_MATERIAL_SLOT)  /// @note MaterialConstants per-material
#define CB_LIGHT        FBZZ_REGISTER(b, CB_LIGHT_SLOT)  /// @note LightConstants    per-frame
#define CB_SHADOW       FBZZ_REGISTER(b, CB_SHADOW_SLOT)  /// @note ShadowConstants   per-frame
#define CB_POSTPROC     FBZZ_REGISTER(b, CB_POSTPROC_SLOT)  /// @note PostProcConstants per-pass
#define CB_SKINNING     FBZZ_REGISTER(b, CB_SKINNING_SLOT)  /// @note SkinningConstants per-animated draw
#define CB_ATMOSPHERE   FBZZ_REGISTER(b, CB_ATMOSPHERE_SLOT)  /// @note AtmosphereConstants per-frame (Skydome のみ)

/// @note Velocity パス専用: 前フレームのボーンパレット。
/// @note b2 に相乗りするか: b0〜b13 は全て別用途に埋まっている。Velocity パスは
/// @note 速度しか書かずマテリアルを一切読まないので、このパスに限り b2 が空く。
/// @note Velocity.hlsl / VelocitySkinned.hlsl 以外がこの名前を使ってはいけない。
#define CB_PREV_SKINNING FBZZ_REGISTER(b, CB_PREV_SKINNING_SLOT)

/// @note ---- UI (UISystem / UI マテリアル) -----------------------------------
/// @note UI パスは b0 を CameraConstants ではなく UIConstants として使う。
/// @note 専用スロットへ逃がさないか:
/// @note UI の描画に view / projection は要らない。矩形は Canvas 空間で組み立て、
/// @note ortho 1 本で clip 空間へ送る。カメラ行列を束縛しないパスで b0 を空けておく
/// @note 意味が無く、逆に UI だけ離れた番号を使うと DrawCall の埋め方が UI だけ
/// @note 例外になる。代わりに UI シェーダーは Constants.hlsli を include しない、
/// @note という制約を UICommon.hlsli の #error で機械的に守らせる。
#define CB_UI           FBZZ_REGISTER(b, CB_UI_SLOT)  /// @note UIConstants per-draw (ortho / color / uvRect / rect)
                            /// @note マテリアルパラメータは CB_MATERIAL (b2) を使う。
                            /// @note 名前も MaterialConstants に揃えること — シェーダー
                            /// @note リフレクションは cbuffer 名で探すため、名前を変えると
                            /// @note .mat の params と Inspector が丸ごと効かなくなる。

/// @note ---- Decal (DecalPass) -----------------------------------------------
/// @note 投影ボリューム・角度フェード・受信レイヤーはエンジンが毎ドロー埋める。
///
/// @note b2 に置かないか:
/// @note シェーダーリフレクションは cbuffer 名 "MaterialConstants" を探す
/// @note (DX11Shader/DX12Shader::BuildDescriptor)。デカール定数をそこへ置くと
/// @note .mat の params を 1 つも束縛できず、デカールだけマテリアルを持てない
/// @note 例外になる。b2 は材質へ明け渡し、エンジン側は空きスロットへ逃がす。
#define CB_DECAL        FBZZ_REGISTER(b, CB_DECAL_SLOT) /// @note DecalConstants per-decal
                            /// @note デカール材質のパラメータは CB_MATERIAL (b2)。
                            /// @note 契約は Material/Decal/DecalCommon.hlsli が持つ。

/// @note ---- Particle (ParticlePass) -----------------------------------------
/// @note ビルボードの展開方法・フリップブック・歪み・煙・自己影といった、エンジンが
/// @note エミッターごとに決める値。材質側からは読み取り専用。
///
/// @note b2 に置かないか:
/// @note CB_DECAL とまったく同じ理由。リフレクションは cbuffer 名 "MaterialConstants" を
/// @note b2 に探すため、ここを占有するとパーティクル材質だけ .mat の [params] を
/// @note 1 つも持てない例外になる。b2 は材質へ明け渡し、エンジン側は空きスロットへ逃がす。
#define CB_PARTICLE     FBZZ_REGISTER(b, CB_PARTICLE_SLOT) /// @note ParticleRenderConstants per-emitter
                            /// @note パーティクル材質のパラメータは CB_MATERIAL (b2)。
                            /// @note 契約は Material/Effects/ParticleMaterial.hlsli が持つ。

/// @note 残像 1 枚ぶんの色 (古いサンプルほど colorEnd 寄り)。エンジンが毎ドロー埋める。
///
/// @note b2 に置かないか:
/// @note CB_DECAL / CB_PARTICLE とまったく同じ理由。リフレクションは cbuffer 名
/// @note "MaterialConstants" を b2 に探すため、ここを占有すると残像材質だけ .mat の
/// @note [params] を 1 つも持てない例外になる。
/// @note 新しい番号を作らず b6 を借りるか:
/// @note b0〜b13 は既に全て別用途で埋まっている。残像の描画は大気も空も読まないので、
/// @note このパスに限り b6 が空く (CB_PREV_SKINNING が b2 を借りるのと同じ作法)。
/// @note MeshTrail / SkinnedMeshTrail とその材質シェーダー以外がこの名前を使ってはいけない。
#define CB_MESHTRAIL    FBZZ_REGISTER(b, CB_MESHTRAIL_SLOT)  /// @note MeshTrailConstants per-sample
                            /// @note 残像材質のパラメータは CB_MATERIAL (b2)。
                            /// @note 契約は Material/Effects/MeshTrailMaterial.hlsli が持つ。

/// @note ---- Texture (マテリアル, per-draw) ----------------------------------
#define TEX_ALBEDO          FBZZ_REGISTER(t, TEX_ALBEDO_SLOT)
#define TEX_NORMAL          FBZZ_REGISTER(t, TEX_NORMAL_SLOT)
#define TEX_METALLIC_ROUGH  FBZZ_REGISTER(t, TEX_METALLIC_ROUGH_SLOT)  /// @note R=metallic  G=roughness
#define TEX_EMISSIVE        FBZZ_REGISTER(t, TEX_EMISSIVE_SLOT)
#define TEX_AO              FBZZ_REGISTER(t, TEX_AO_SLOT)

/// @note ---- Texture (パスリソース, per-pass) --------------------------------
#define TEX_GBUFFER0    FBZZ_REGISTER(t, TEX_GBUFFER0_SLOT)   /// @note albedo(RGB) + roughness(A)
#define TEX_GBUFFER1    FBZZ_REGISTER(t, TEX_GBUFFER1_SLOT)   /// @note normal(RGB) + metallic(A)
#define TEX_DEPTH       FBZZ_REGISTER(t, TEX_DEPTH_SLOT)
#define TEX_SHADOW      FBZZ_REGISTER(t, TEX_SHADOW_SLOT)
#define TEX_SSAO        FBZZ_REGISTER(t, TEX_SSAO_SLOT)
#define TEX_BLOOM       FBZZ_REGISTER(t, TEX_BLOOM_SLOT)
#define TEX_PARTICLE_SIX_WAY_COLOR FBZZ_REGISTER(t, TEX_PARTICLE_SIX_WAY_COLOR_SLOT) /// @note Particle pass の six-way albedo hue atlas
#define TEX_ENV_CUBE    FBZZ_REGISTER(t, TEX_ENV_CUBE_SLOT)  /// @note Skybox キューブマップ / IBL
#define TEX_PARTICLE_SIX_WAY_EMISSION FBZZ_REGISTER(t, TEX_PARTICLE_SIX_WAY_EMISSION_SLOT) /// @note Particle pass の six-way emission atlas
#define TEX_ENV_EQUIRECT FBZZ_REGISTER(t, TEX_ENV_EQUIRECT_SLOT) /// @note Skydome 等緯度テクスチャ
/// @note カスタムパスが宣言したときだけ束縛される追加入力。IBL / Skydome と時分割で共有する。
/// @note 空きスロットを増やさず時分割か: 全画面のカスタムパスはシーンのジオメトリを
/// @note 描かないので環境マップを読まない。同じフレームでも同時に束縛されることが無い。
#define TEX_CUSTOM_VELOCITY FBZZ_REGISTER(t, TEX_CUSTOM_VELOCITY_SLOT) /// @note モーションベクター (RG = 速度)
#define TEX_CUSTOM_NORMAL   FBZZ_REGISTER(t, TEX_CUSTOM_NORMAL_SLOT) /// @note GBuffer 法線 (Deferred 系のみ)
/// @note 各ピクセルの可視サーフェスが属するレイヤー番号 + 1 を格納する。0 = 未描画。
/// @note 0 を「未描画」に使うか: レイヤー 0 (Default) が有効な番号なので、クリア値と
/// @note 区別が付かないと地形やフォリッジのように受信バッファへ描かないジオメトリが
/// @note すべて Default 扱いになる。+1 しておけば「不明なら受信させる」に倒せる。
#define TEX_DECAL_MASK   FBZZ_REGISTER(t, TEX_DECAL_MASK_SLOT) /// @note デカール受信レイヤーバッファ (レイヤーフィルタ使用時のみ)
/// @note パーティクル自己影の光源側密度バッファ (R=Σα, G=Σα·深度)。
/// @note SSAO と同じ t9 を時分割で使う。パーティクル描画は SSAO を読まないため衝突しない。
#define TEX_PARTICLE_DENSITY FBZZ_REGISTER(t, TEX_PARTICLE_DENSITY_SLOT)

/// @note ---- StructuredBuffer (t14〜: テクスチャ SRV と重複しない領域) --------
#define SB_GPU_PARTICLES    FBZZ_REGISTER(t, SB_GPU_PARTICLES_SLOT)  /// @note StructuredBuffer<GpuParticle> (VS 描画用 / CS RW 用)
#define SB_GPU_SPAWN        FBZZ_REGISTER(t, SB_GPU_SPAWN_SLOT)  /// @note StructuredBuffer<GpuSpawnEntry> (CS スポーン入力)
/// @note GPU ソート済みの (key, particleIndex)。t15 を CS のスポーン入力と時分割で共有する。
/// @note スポーン CS と描画 VS は別ステージ・別ディスパッチで、同時にバインドされることがない。
/// @note t16 以降は Advanced Graphics が使い切っているため、空きスロットを増やすより時分割が安い。
#define SB_GPU_SORT         FBZZ_REGISTER(t, SB_GPU_SORT_SLOT)  /// @note VS: StructuredBuffer<uint2> (ソート済みインデックス)

/// @note 粒子に効く流れ一式 (内蔵 + シーン)。StructuredBuffer<GpuFlowField>。
/// @note 定数バッファではないか: cbuffer は固定長なので «渡せる力の本数» に上限が要り、
/// @note あふれた分は捨てるしかない。捨てられたのが重力だと粒子がその場に浮くという、
/// @note 設定ミスと区別の付かない壊れ方をする。SRV にすれば上限そのものが消える。
/// @note SB_PUNCTUAL_LIGHTS と同じ t29 を時分割する (粒子 CS はライトを読まない)。
#define SB_PARTICLE_FORCES  FBZZ_REGISTER(t, SB_PARTICLE_FORCES_SLOT)

/// @note ---- UAV (コンピュートシェーダー出力) ---------------------------------
#define UAV_OUTPUT      FBZZ_REGISTER(u, UAV_OUTPUT_SLOT)
#define UAV_OUTPUT2     FBZZ_REGISTER(u, UAV_OUTPUT2_SLOT)
#define UAV_GPU_PARTICLES   FBZZ_REGISTER(u, UAV_GPU_PARTICLES_SLOT)   /// @note RWStructuredBuffer<GpuParticle> (CS 書き込み)
/// @note GPU パーティクルのソートキー書き込み先。キー生成 CS では u2 (粒子 SRV) と同時に使うため u3 へ置く。
/// @note UAV_SSR / UAV_CONTACT_SHADOW と同じスロットだが、いずれも逐次パスで同時実行しない。
#define UAV_GPU_SORT        FBZZ_REGISTER(u, UAV_GPU_SORT_SLOT)   /// @note RWStructuredBuffer<uint2> (key, particleIndex)
/// @note コンピュートスキニングの出力頂点。CS が書き、以降のパスは同じバッファを
/// @note 通常の頂点バッファとして読む (スキニングを 1 フレーム 1 回だけにするため)。
#define UAV_SKINNED_VERTICES FBZZ_REGISTER(u, UAV_SKINNED_VERTICES_SLOT)  /// @note RWStructuredBuffer<SkinnedOutVertex>

/// @note ---- SRV (コンピュートスキニング入力) ---------------------------------
/// @note ComputeCall::srvBuffers[14] / [15] が束縛される (添字 = レジスタ番号)。GPU パーティクルの
/// @note SB_GPU_PARTICLES / SB_GPU_SORT と同じスロットだが、別ディスパッチなので競合しない。
#define SB_SKIN_SRC_VERTICES FBZZ_REGISTER(t, SB_SKIN_SRC_VERTICES_SLOT) /// @note StructuredBuffer<SkinSrcVertex>
#define SB_SKIN_BONES        FBZZ_REGISTER(t, SB_SKIN_BONES_SLOT) /// @note StructuredBuffer<float4x4> (ボーンパレット)

/// @note ---- Sampler ---------------------------------------------------------
#define SAMPLER_DEFAULT      FBZZ_REGISTER(s, SAMPLER_DEFAULT_SLOT)
#define SAMPLER_SHADOW       FBZZ_REGISTER(s, SAMPLER_SHADOW_SLOT)   /// @note SamplerComparisonState (PCF 用)
#define SAMPLER_LINEAR_CLAMP FBZZ_REGISTER(s, SAMPLER_LINEAR_CLAMP_SLOT)   /// @note Linear clamp (IBL BRDF LUT / 3D LUT 用)
#define SAMPLER_POINT_CLAMP  FBZZ_REGISTER(s, SAMPLER_POINT_CLAMP_SLOT)   /// @note Point clamp  (TAA 再投影ルックアップ用)
#define SAMPLER_WRAP_LINEAR  FBZZ_REGISTER(s, SAMPLER_WRAP_LINEAR_SLOT)   /// @note Linear wrap  (ボリューメトリック雲のタイラブル 3D ノイズ用)
/// @note s5〜s8 は DX12PsoCache::MakeStaticSamplers() が静的サンプラーとして常時焼いている。
/// @note 定義がここに無いと「どのフィルタが来るか」がバックエンドのコードを読まないと分からない。
#define SAMPLER_UI           FBZZ_REGISTER(s, SAMPLER_UI_SLOT)   /// @note Linear clamp (UI スプライト / テキスト)
/// @note Cookie アトラス用。設定は SAMPLER_UI と同じ Linear clamp なので同じ枠を使う。
/// @note 同居できるか: UI シェーダーは Constants.hlsli / Lighting.hlsli を include しない
/// @note 規約 (UICommon.hlsli の #error が機械的に守らせている) ため、s5 を宣言する
/// @note シェーダーは「UI か、ライティングを持つマテリアルか」のどちらか一方に必ずなる。
/// @note s2 (SAMPLER_LINEAR_CLAMP) を使わないか: あちらは IBL BRDF LUT 用に
/// @note マテリアルシェーダーが自前の名前で宣言済み。共有ヘッダーから重ねて宣言できない。
#define SAMPLER_COOKIE       FBZZ_REGISTER(s, SAMPLER_COOKIE_SLOT)   /// @note Linear clamp (ライト Cookie)
#define SAMPLER_UI_POINT     FBZZ_REGISTER(s, SAMPLER_UI_POINT_SLOT)   /// @note Point  clamp (ドット絵 UI / 拡大時に補間させたくない図版)
/// @note Spot / Point シャドウ用の 2 本目の比較サンプラー。設定は s1 と同一。
/// @note s1 を使い回さないか: s1 の SamplerComparisonState は各マテリアルシェーダーが
/// @note 自前の名前で宣言している (texShadow / sampShadow / gSampShadow …)。
/// @note 共有ヘッダーからもう 1 つ s1 を宣言するとレジスタが二重定義になり、
/// @note かといって呼び出し側の名前は統一されていないので参照もできない。
/// @note 専用スロットを 1 本使うのが、40 以上のシェーダーを書き換えずに済む唯一の道。
#define SAMPLER_SHADOW_PUNCTUAL FBZZ_REGISTER(s, SAMPLER_SHADOW_PUNCTUAL_SLOT) /// @note SamplerComparisonState (Spot / Point の PCF 用)
/// @note 地形レイヤー (diffuse / normal / aoRoughness) のタイリング用。
/// @note s0 と分けるか: 地形は画面を広く覆い、レイヤーごとに 3 枚を引くので x16 異方性の
/// @note コストが枚数ぶん乗る。x4 なら見た目はほぼ変わらず約 1/3 で済む。
#define SAMPLER_WRAP_ANISO4  FBZZ_REGISTER(s, SAMPLER_WRAP_ANISO4_SLOT)   /// @note Aniso x4 wrap (地形レイヤー)

/// @note ---- Advanced Graphics (IBL / SSR / TAA / GTAO 等) ----
#define CB_ADVANCED_GRAPHICS FBZZ_REGISTER(b, CB_ADVANCED_GRAPHICS_SLOT)

#define TEX_IBL_IRRADIANCE   FBZZ_REGISTER(t, TEX_IBL_IRRADIANCE_SLOT)  /// @note Diffuse IBL 事前畳み込みキューブマップ
#define TEX_IBL_PREFILTER    FBZZ_REGISTER(t, TEX_IBL_PREFILTER_SLOT)  /// @note Specular IBL 事前フィルタ済みキューブマップ (roughness → mip)
#define TEX_IBL_BRDF_LUT     FBZZ_REGISTER(t, TEX_IBL_BRDF_LUT_SLOT)  /// @note BRDF 積分 LUT (x=scale, y=bias)

#define TEX_SSR              FBZZ_REGISTER(t, TEX_SSR_SLOT)  /// @note SSR Compute 出力
#define TEX_VOLUMETRIC       FBZZ_REGISTER(t, TEX_VOLUMETRIC_SLOT)  /// @note ボリューメトリックライト Compute 出力
#define TEX_TAA_HISTORY      FBZZ_REGISTER(t, TEX_TAA_HISTORY_SLOT)  /// @note TAA 前フレームカラーバッファ
#define TEX_LUT_COLOR_GRADE  FBZZ_REGISTER(t, TEX_LUT_COLOR_GRADE_SLOT)  /// @note 3D カラーグレーディング LUT
/// @note Light Probe Volume の SH 係数 (Texture3D, Z 方向に 7 枚を積む)。
/// @note TEX_LUT_COLOR_GRADE と時分割か: t22 を読むのは Composite だけで、ライティングを
/// @note 持つマテリアル / DeferredLighting は宣言していない。どちらも Texture3D なので
/// @note null ディスクリプタの次元も食い違わない (TEX_VELOCITY_FIELD と同じ組み方)。
#define TEX_LIGHT_PROBE_SH   FBZZ_REGISTER(t, TEX_LIGHT_PROBE_SH_SLOT)
/// @note 2 つ目 (外側) の Light Probe Volume。t21 を読むのは TAA パスだけで、ライティングを持つシェーダーは宣言していない。
/// @note TAA の Texture2D と次元が違うが、bindless では添字で引くので null ディスクリプタの次元は問題にならない (無効時は b8 の intensity で引かない)。
#define TEX_LIGHT_PROBE_SH_OUTER FBZZ_REGISTER(t, TEX_LIGHT_PROBE_SH_OUTER_SLOT)
#define TEX_GTAO             FBZZ_REGISTER(t, TEX_GTAO_SLOT)  /// @note GTAO 結果 (SSAO の代替)
/// @note Forward のマテリアルシェーダーが読む「統一済み画面空間 AO」(GTAO か SSAO のどちらか)。
///
/// @note TEX_SSAO (t9) を使わないか: t9 は Terrain が AO/Roughness レイヤー配列に、
/// @note Water がシャドウマップに、パーティクルが自己影の密度バッファに使っており、
/// @note マテリアル側では空いていない。t23 はマテリアル系のどのシェーダーも宣言していない。
/// @note TEX_GTAO と同じ番号でよいか: t23 を読むのは GTAO のブラー CS (中間出力) と
/// @note Composite (froxel fog) だけで、どちらもマテリアル描画とは別のディスパッチ / パス。
/// @note 同時に束縛されることがない。
#define TEX_SCREEN_AO        FBZZ_REGISTER(t, TEX_SCREEN_AO_SLOT)
#define TEX_CONTACT_SHADOW   FBZZ_REGISTER(t, TEX_CONTACT_SHADOW_SLOT)  /// @note コンタクトシャドウマスク
#define TEX_SCENE_DEPTH      FBZZ_REGISTER(t, TEX_SCENE_DEPTH_SLOT)  /// @note Forward 不透明物を含む最終シーン深度 (SSR 遮蔽用)
#define TEX_CLOUD_SHAPE      FBZZ_REGISTER(t, TEX_CLOUD_SHAPE_SLOT)  /// @note ボリューメトリック雲 Shape 3D ノイズ (128³ Perlin-Worley)
#define TEX_CLOUD_DETAIL     FBZZ_REGISTER(t, TEX_CLOUD_DETAIL_SLOT)  /// @note ボリューメトリック雲 Detail 3D ノイズ (32³ Worley 高周波)

/// @note 常駐中の 速度場 PNG を積んだ Texture3D アトラス (32³ タイル × N)。
/// @note TEX_CLOUD_SHAPE と t26 を時分割する。**どちらも Texture3D** なので、
/// @note DX12 の null ディスクリプタ次元が食い違う心配が無い組み合わせになっている。
#define TEX_VELOCITY_FIELD   FBZZ_REGISTER(t, TEX_VELOCITY_FIELD_SLOT)

/// @note モーションベクター: RG = 現 UV - 前フレーム UV、B = 書き込み済みフラグ (1)。
/// @note B が 0 の画素 (空・未描画) では読み手が従来どおり深度再投影へフォールバックする。
///
/// @note TEX_CLOUD_SHAPE と時分割か: DrawCall::textures は t0〜t31 の 32 枠で空きが無い。
/// @note 枠を増やすと DX12 のルートシグネチャ (srvRange.NumDescriptors) の変更になり
/// @note 全 PSO へ波及する。雲 3D ノイズを読むのは VolumetricCloud.hlsl だけで、
/// @note TAA / MotionBlur / Velocity は雲を一切参照しないため同時に束縛されない。
/// @note TEX_FROXEL_SCATTER が TEX_VOLUMETRIC と時分割しているのと同じ扱い。
#define TEX_VELOCITY         FBZZ_REGISTER(t, TEX_VELOCITY_SLOT)

/// @note ---- Spot / Point シャドウ + Cookie -----------------------------------
/// @note Directional の CSM (TEX_SHADOW) とは別アトラス。
/// @note 相乗りさせないか: CSM は「カメラ視錐台をどう分割するか」でタイル数が決まるのに対し、
/// @note こちらは「シーンにいくつ影付きライトがあるか」で決まる。同じ面積を奪い合わせると、
/// @note ライトを 1 つ置いただけで Directional の遠景カスケードが粗くなる。
#define TEX_PUNCTUAL_SHADOW  FBZZ_REGISTER(t, TEX_PUNCTUAL_SHADOW_SLOT)  /// @note Spot / Point シャドウアトラス (深度)
/// @note スポット / 点光源へ被せる投影テクスチャ (Unity の Cookie / UE の Light Function)。
/// @note 単一アトラスに複数 Cookie を敷き詰め、ライトごとに矩形で切り出す。
#define TEX_LIGHT_COOKIE     FBZZ_REGISTER(t, TEX_LIGHT_COOKIE_SLOT)  /// @note Cookie アトラス (RGBA)

#define UAV_SSR              FBZZ_REGISTER(u, UAV_SSR_SLOT)   /// @note SSR Compute 書き込み先
#define UAV_VOLUMETRIC       FBZZ_REGISTER(u, UAV_VOLUMETRIC_SLOT)   /// @note ボリューメトリック Compute 書き込み先
#define UAV_MOTION_BLUR      FBZZ_REGISTER(u, UAV_MOTION_BLUR_SLOT)   /// @note モーションブラー Compute 書き込み先
#define UAV_GTAO_RAW         FBZZ_REGISTER(u, UAV_GTAO_RAW_SLOT)   /// @note GTAO RAW Compute 書き込み先
#define UAV_GTAO_BLUR        FBZZ_REGISTER(u, UAV_GTAO_BLUR_SLOT)   /// @note GTAO ブラー後 Compute 書き込み先
/// @note DX11 SM5.0 の CS UAV スロットは u0〜u7 の 8 本しかない。
/// @note 下記 2 パスはどちらも他の CS と同時実行しない逐次パスなので、
/// @note 既存スロットを再利用することで上限内に収める。
#define UAV_CONTACT_SHADOW   FBZZ_REGISTER(u, UAV_CONTACT_SHADOW_SLOT)   /// @note コンタクトシャドウ Compute 書き込み先 (UAV_SSR と時分割で再利用)
#define UAV_BRDF_LUT         FBZZ_REGISTER(u, UAV_BRDF_LUT_SLOT)   /// @note BRDF LUT ベイク出力 (起動時 1 回のみ。UAV_OUTPUT と時分割で再利用)

/// @note ---- フロクセル ボリューメトリック フォグ ------------------------------
/// @note 視錐台を XY タイル × 対数 Z スライスへ切ったボリューム。2 パスで作る。
/// @note 1. Inject    : 各フロクセルの散乱色と消散係数を求めて gFroxelScatter へ書く
/// @note 2. Integrate : Z 方向へ前から積分し、透過率込みの結果を gFroxelIntegrated へ書く
/// @note 適用は Composite が深度からスライスを引いて 1 回サンプルするだけ。
///
/// @note u0/u1 を使い回すか: どちらのパスも他の CS と同時実行しない逐次パスで、
/// @note DX11 SM5.0 の CS UAV は u0〜u7 しかない。専用スロットを増やすより時分割が安い。
#define UAV_FROXEL_SCATTER    FBZZ_REGISTER(u, UAV_FROXEL_SCATTER_SLOT)  /// @note RWTexture3D<float4> (UAV_OUTPUT と時分割)
#define UAV_FROXEL_INTEGRATED FBZZ_REGISTER(u, UAV_FROXEL_INTEGRATED_SLOT)  /// @note RWTexture3D<float4> (UAV_OUTPUT2 と時分割)

/// @note Bloom アップサンプルが「同じ寸法のダウンサンプル結果」を足すための入力。
/// @note 書き込み先の UAV から読まないか: コンピュート用テクスチャは RGBA16F で、
/// @note D3D11 の typed UAV load が保証されるのは R32 系だけ。RWTexture2D<float4> を
/// @note 読むと未定義動作になり、実際に画面が 2x2 に割れる形で壊れた。
/// @note 読むものは必ず SRV から読む。t9 (TEX_SSAO) は Bloom の経路では使われない。
#define TEX_BLOOM_ADD         FBZZ_REGISTER(t, TEX_BLOOM_ADD_SLOT)

#define TEX_FROXEL_SCATTER    FBZZ_REGISTER(t, TEX_FROXEL_SCATTER_SLOT) /// @note Integrate の入力 (TEX_VOLUMETRIC と時分割: 同時に読まない)
#define TEX_FROXEL_FOG        FBZZ_REGISTER(t, TEX_FROXEL_FOG_SLOT) /// @note Composite が読む積分済みボリューム (TEX_GTAO と時分割)

/// @note Inject が読む前フレームの散乱ボリューム。t20 を TEX_FROXEL_SCATTER と時分割する。
/// @note 同じ番号でよいか: Inject と Integrate は別のディスパッチで、どちらも
/// @note 「そのパスが読むフロクセルボリューム」を t20 に置く。同時には読まない。
/// @note サンプラーは 3D の linear clamp が要るだけなので s2 を共用する (s5/s7 は満杯)。
#define TEX_FROXEL_HISTORY     FBZZ_REGISTER(t, TEX_FROXEL_HISTORY_SLOT)
#define SAMPLER_FROXEL_HISTORY SAMPLER_LINEAR_CLAMP

/// @note フロクセル霧の定数。
#define CB_FROXEL_FOG        FBZZ_REGISTER(b, CB_FROXEL_FOG_SLOT) /// @note FroxelFogConstants

/// @note bindless ディスクリプタ添字ブロック。全シェーダーが同じ並びで受け取る。
/// @note 詳細は Common/BindlessIndices.hlsli と Docs/design/bindless.md
#define CB_BINDLESS_INDICES  FBZZ_REGISTER(b, CB_BINDLESS_INDICES_SLOT)

/// @note ---- Clustered Lighting (Forward+ / Deferred+) ------------------------
#define CB_CLUSTER           FBZZ_REGISTER(b, CB_CLUSTER_SLOT)   /// @note ClusterConstants (グリッド寸法 / Z スライス係数 / ライト数)

/// @note ---- Spot / Point シャドウ + Cookie の定数 ----------------------------
/// @note b4 (ShadowConstants) へ足さないか: あちらは 464 bytes 固定で C++ 側に
/// @note static_assert があり、Terrain / Water / パーティクルまで含む 20 以上の
/// @note シェーダーが同じレイアウトを読む。1 つ追加するたびに全経路の再検証が要る。
/// @note Spot / Point は「読むシェーダーが増えていく途中」なので、別 cbuffer にして
/// @note 束縛していないパスが 0 埋め (= 影なし) で素通りできるようにする。
#define CB_PUNCTUAL_SHADOW   FBZZ_REGISTER(b, CB_PUNCTUAL_SHADOW_SLOT)  /// @note PunctualShadowConstants (VP / アトラス矩形 / バイアス / Cookie)

/// @note ライトデータ。DrawCall::psBuffers[0..1] と ComputeCall::srvBuffers[29] / [30] が束縛される。
/// @note Graphics/Renderer/DrawCall.hpp の kPsBufferBaseSlot (=29) と一致させること。
/// @note t14/t15 を使わないか: あちらは両バックエンドとも頂点シェーダー専用スロットで、
/// @note PS からは読めない (DX11 は VSSetShaderResources、DX12 の root param 15 は VERTEX 可視)。
/// @note フロクセル霧の Inject CS もここを読む。CS の SRV テーブルは t0〜t31 を覆うので、
/// @note PS と同じレジスタのまま ComputeCall::srvBuffers 経由で渡せる。
#define SB_PUNCTUAL_LIGHTS   FBZZ_REGISTER(t, SB_PUNCTUAL_LIGHTS_SLOT)  /// @note StructuredBuffer<PunctualLight>  点光源+スポットを統合した配列
#define SB_CLUSTER_INDICES   FBZZ_REGISTER(t, SB_CLUSTER_INDICES_SLOT)  /// @note StructuredBuffer<uint>  [cluster*stride] = 個数, 続けてライト番号

/// @note カリング CS の入力。ComputeCall::srvBuffers[14] が束縛される。
/// @note GPU パーティクル / スキニングと同じスロットだが、別ディスパッチなので競合しない。
#define SB_CLUSTER_LIGHTS_CS FBZZ_REGISTER(t, SB_CLUSTER_LIGHTS_CS_SLOT)  /// @note StructuredBuffer<PunctualLight>

/// @note カリング CS の出力。ComputeCall::uavBuffers[0] が u2 へ束縛される。
/// @note UAV が 1 本で足りるか: 1 スレッド = 1 クラスタにするとカウンタがスレッド内で完結するため、
/// @note InterlockedAdd も offset テーブルも不要になり、固定ストライドで直接書ける。
/// @note おかげで UAV_SSR / UAV_CONTACT_SHADOW / UAV_GPU_SORT が密集する u3 を避けられる。
#define UAV_CLUSTER_LIGHTS   FBZZ_REGISTER(u, UAV_CLUSTER_LIGHTS_SLOT)   /// @note RWStructuredBuffer<uint> (UAV_GPU_PARTICLES と時分割で再利用)


/// @note DrawCall・ComputeCall と bindless 添字ブロックの幅。部分配列は公開 ABI の範囲。
#define BINDING_COUNT_CONSTANT_BUFFERS 14
#define BINDING_COUNT_PIXEL_SRVS 32
#define BINDING_COUNT_VERTEX_SRVS 4
#define BINDING_COUNT_UAVS 8
#define BINDING_COUNT_BUFFER_RANGE 2

/// @brief 定数バッファ。異なるパスで同じ番号を再利用できる。
#define CB_ADVANCED_GRAPHICS_SLOT                        8
#define CB_ATMOSPHERE_SLOT                               6
#define CB_BAKE_REPLAY_SLOT                              0
#define CB_BINDLESS_INDICES_SLOT                         14
#define CB_CAMERA_SLOT                                   0
#define CB_CLUSTER_SLOT                                  9
#define CB_DECAL_SLOT                                    10
#define CB_FIBER_CONTACT_SLOT                            10
#define CB_FIBER_FRAME_SLOT                              5
#define CB_FIBER_MOTION_SLOT                             6
#define CB_FIBER_PREVIOUS_SKINNING_SLOT                  9
#define CB_FLIPBOOK_COMPARE_SLOT                         0
#define CB_FLUID_PASS_SLOT                               1
#define CB_FLUID_STEP_SLOT                               0
#define CB_FROXEL_FOG_SLOT                               13
#define CB_HIZ_BUILD_SLOT                                0
#define CB_IBL_FACE_SLOT                                 0
#define CB_IBL_PREFILTER_SLOT                            0
#define CB_INSTANCE_DRAW_OFFSET_SLOT                     15
#define CB_LIGHT_SLOT                                    3
#define CB_LIGHT_PROBE_DILATE_SLOT                       0
#define CB_LIGHT_PROBE_PROJECT_SLOT                      0
#define CB_LIQUID_PASS_SLOT                              1
#define CB_LIQUID_STEP_SLOT                              0
#define CB_MANY_LIGHT_PROBE_SLOT                         13
#define CB_MATERIAL_SLOT                                 2
#define CB_MESHLET_GEOMETRY_SLOT                         13
#define CB_MESHTRAIL_SLOT                                6
#define CB_MESH_OCCLUSION_VIEW_SLOT                      0
#define CB_OBJECT_SLOT                                   1
#define CB_PARTICLE_SLOT                                 11
#define CB_PARTICLE_EMITTER_SLOT                         0
#define CB_PARTICLE_SORT_SLOT                            0
#define CB_POSTPROC_SLOT                                 5
#define CB_PREVIEW_SLOT                                  5
#define CB_PREV_SKINNING_SLOT                            2
#define CB_PUNCTUAL_SHADOW_SLOT                          12
#define CB_RAY_DEBUG_SLOT                                0
#define CB_RAY_GAME_TRACE_SLOT                           1
#define CB_RAY_PATH_TRACE_SLOT                           0
#define CB_RAY_RECONSTRUCTION_SLOT                       0
#define CB_RAY_REFLECTION_SLOT                           0
#define CB_RAY_REFLECTION_RECONSTRUCTION_SLOT            0
#define CB_SHADOW_SLOT                                   4
#define CB_SKINNING_SLOT                                 7
#define CB_SKIN_COMPUTE_SLOT                             0
#define CB_TAA_CYCLE_SLOT                                2
#define CB_UI_SLOT                                       0
#define CB_VOLUMETRIC_CLOUD_SLOT                         2
#define CB_VOLUME_FILL_SLOT                              0
#define CB_VOLUME_RAYMARCH_SLOT                          0
#define CB_VOLUME_UPLOAD_SLOT                            0
#define CB_WATER_SLOT                                    1
#define CB_WATER_MEDIUM_CAPTURE_SLOT                     10
#define CB_WATER_MEDIUM_PROBE_SLOT                       13
#define CB_WATER_MESH_SLOT                               13
#define CB_WATER_ZONE_APPEARANCE_SLOT                    11

/// @brief テクスチャ・バッファ・加速構造の SRV。異なるパスで同じ番号を再利用できる。
#define TEX_ALBEDO_SLOT                                  0
#define TEX_AO_SLOT                                      4
#define SRV_AUXILIARY_FIRST_SLOT                         14
#define SRV_BAKED_DEPTH_SLOT                             0
#define SRV_BAKED_DEPTH_PACKED_SLOT                      0
#define SRV_BAKE_BINDINGS_SLOT                           29
#define SRV_BAKE_NODES_SLOT                              15
#define SRV_BAKE_ROWS_SLOT                               30
#define SRV_BAKE_SAMPLES_SLOT                            14
#define TEX_BLOOM_SLOT                                   10
#define TEX_BLOOM_ADD_SLOT                               9
#define SRV_CAUSTICS_SLOT                                0
#define TEX_CLOUD_DETAIL_SLOT                            27
#define SRV_CLOUD_HALF_SLOT                              0
#define TEX_CLOUD_SHAPE_SLOT                             26
#define SB_CLUSTER_INDICES_SLOT                          30
#define SB_CLUSTER_LIGHTS_CS_SLOT                        14
#define TEX_CONTACT_SHADOW_SLOT                          24
#define SRV_CULL_CANDIDATES_SLOT                         14
#define SRV_CULL_VIEW_SLOT                               15
#define TEX_CUSTOM_NORMAL_SLOT                           12
#define TEX_CUSTOM_VELOCITY_SLOT                         11
#define TEX_DECAL_MASK_SLOT                              13
#define TEX_DEPTH_SLOT                                   7
#define TEX_EMISSIVE_SLOT                                3
#define TEX_ENV_CUBE_SLOT                                11
#define TEX_ENV_EQUIRECT_SLOT                            12
#define SRV_EXPOSURE_RESULT_SLOT                         29
#define SRV_FIBER_MASK_SLOT                              5
#define SRV_FLIPBOOK_COLOR_SLOT                          0
#define SRV_FLIPBOOK_MOTION_SLOT                         1
#define SRV_FLUID_ADVECT_SCALAR_FIELD_IN_SLOT            0
#define SRV_FLUID_ADVECT_SCALAR_VELOCITY_IN_SLOT         1
#define SRV_FLUID_ADVECT_VELOCITY_SOLID_IN_SLOT          1
#define SRV_FLUID_ADVECT_VELOCITY_VELOCITY_IN_SLOT       0
#define SRV_FLUID_CONFINE_CURL_IN_SLOT                   1
#define SRV_FLUID_CONFINE_VELOCITY_IN_SLOT               0
#define SRV_FLUID_CORRECT_BACKWARD_IN_SLOT               2
#define SRV_FLUID_CORRECT_FORWARD_IN_SLOT                1
#define SRV_FLUID_CORRECT_ORIGINAL_IN_SLOT               0
#define SRV_FLUID_CORRECT_SOLID_IN_SLOT                  4
#define SRV_FLUID_CORRECT_VELOCITY_IN_SLOT               3
#define SRV_FLUID_CURL_VELOCITY_IN_SLOT                  0
#define SRV_FLUID_DIVERGENCE_EXPANSION_IN_SLOT           1
#define SRV_FLUID_DIVERGENCE_SOLID_IN_SLOT               2
#define SRV_FLUID_DIVERGENCE_VELOCITY_IN_SLOT            0
#define SRV_FLUID_FORCES_SCALARS_IN_SLOT                 1
#define SRV_FLUID_FORCES_VELOCITY_IN_SLOT                0
#define SRV_FLUID_INJECT_FUEL_COLOR_IN_SLOT              4
#define SRV_FLUID_INJECT_SCALARS_IN_SLOT                 0
#define SRV_FLUID_INJECT_SOLID_IN_SLOT                   2
#define SRV_FLUID_INJECT_SOURCE_MASKS_SLOT               3
#define SRV_FLUID_INJECT_VELOCITY_IN_SLOT                1
#define SRV_FLUID_JACOBI_DIVERGENCE_IN_SLOT              1
#define SRV_FLUID_JACOBI_PRESSURE_IN_SLOT                0
#define SRV_FLUID_JACOBI_SOLID_IN_SLOT                   2
#define SRV_FLUID_OUTPUT_SCALARS_IN_SLOT                 0
#define SRV_FLUID_OUTPUT_VELOCITY_IN_SLOT                1
#define SRV_FLUID_PROJECT_PRESSURE_IN_SLOT               1
#define SRV_FLUID_PROJECT_SOLID_IN_SLOT                  2
#define SRV_FLUID_PROJECT_VELOCITY_IN_SLOT               0
#define TEX_FROXEL_FOG_SLOT                              23
#define TEX_FROXEL_HISTORY_SLOT                          20
#define TEX_FROXEL_SCATTER_SLOT                          20
#define TEX_GBUFFER0_SLOT                                5
#define TEX_GBUFFER1_SLOT                                6
#define SRV_GBUFFER_EMISSION_SLOT                        22
#define SB_GPU_PARTICLES_SLOT                            14
#define SB_GPU_SORT_SLOT                                 15
#define SB_GPU_SPAWN_SLOT                                15
#define TEX_GTAO_SLOT                                    23
#define TEX_IBL_BRDF_LUT_SLOT                            18
#define TEX_IBL_IRRADIANCE_SLOT                          16
#define TEX_IBL_PREFILTER_SLOT                           17
#define SRV_IBL_SOURCE_SLOT                              0
#define SRV_INSTANCE_BOUNDS_SLOT                         13
#define SRV_INSTANCE_OCCLUSION_SLOT                      30
#define SRV_INSTANCE_PART_LODS_SLOT                      29
#define SRV_LEAF_WETNESS_SLOT                            7
#define TEX_LIGHT_COOKIE_SLOT                            31
#define SRV_LIGHT_PROBE_ENVIRONMENT_SLOT                 16
#define SRV_LIGHT_PROBE_FACE_COLOR_SLOT                  0
#define SRV_LIGHT_PROBE_FACE_DEPTH_SLOT                  6
#define SRV_LIGHT_PROBE_FACE_FACING_SLOT                 20
#define SRV_LIGHT_PROBE_RAW_SH_SLOT                      0
#define TEX_LIGHT_PROBE_SH_SLOT                          22
#define TEX_LIGHT_PROBE_SH_OUTER_SLOT                    21
#define SRV_LIQUID_LAMBDA_SLOT                           30
#define SRV_LIQUID_NEIGHBORS_SLOT               29
#define SRV_LIQUID_SPAWN_SLOT                            29
#define SRV_LIQUID_PARTICLES_SLOT                        14
#define SRV_LIQUID_SORTED_SLOT                           15
#define TEX_LUT_COLOR_GRADE_SLOT                         22
#define SRV_MASK_COLOR_SLOT                              6
#define SRV_MASK_DEPTH_SLOT                              8
#define SRV_MESHLET_HIZ_SLOT                             29
#define SRV_MESHLET_HIZ_DRAW_SLOT                        15
#define SRV_MESHLET_INDEXED_ARGUMENTS_SLOT               30
#define SRV_MESHLET_INSTANCE_IDS_SLOT                    29
#define SRV_MESHLET_INSTANCE_ROWS_SLOT                   15
#define SRV_MESHLET_PAGE_TABLE_SLOT                      14
#define SRV_MESHLET_PAGE_TABLE_CS_SLOT                   15
#define SRV_MESHLET_RAW_DATA_SLOT                        15
#define SRV_MESHLET_RAW_DATA_CS_SLOT                     14
#define SRV_MESHLET_RAW_VERTICES_SLOT                    14
#define TEX_METALLIC_ROUGH_SLOT                          2
#define TEX_NORMAL_SLOT                                  1
#define SRV_OVERDRAW_SLOT                                5
#define SRV_PARALLAX_HEIGHT_SLOT                         5
#define TEX_PARTICLE_DENSITY_SLOT                        9
#define SB_PARTICLE_FORCES_SLOT                          29
#define SRV_PARTICLE_MOTION_SLOT                         6
#define SRV_PARTICLE_NEGATIVE_SLOT                       3
#define SRV_PARTICLE_SCENE_COLOR_SLOT                    5
#define TEX_PARTICLE_SIX_WAY_COLOR_SLOT                  10
#define TEX_PARTICLE_SIX_WAY_EMISSION_SLOT               11
#define SRV_PIXEL_BUFFER_FIRST_SLOT                      29
#define SRV_POST_COLOR_SLOT                              5
#define SB_PUNCTUAL_LIGHTS_SLOT                          29
#define TEX_PUNCTUAL_SHADOW_SLOT                         28
#define SRV_RAY_EMITTERS_SLOT                            3
#define SRV_RAY_GAME_MOTION_SLOT                         12
#define SRV_RAY_HIT_RECORDS_SLOT                         1
#define SRV_RAY_MATERIALS_SLOT                           2
#define SRV_RAY_PATH_ALBEDO_SLOT                         8
#define SRV_RAY_PATH_DEPTH_SLOT                          10
#define SRV_RAY_PATH_EMISSION_SLOT                       11
#define SRV_RAY_PATH_ENVIRONMENT_SLOT                    4
#define SRV_RAY_PATH_ENV_TABLE_SLOT                      7
#define SRV_RAY_PATH_LIGHTS_SLOT                         5
#define SRV_RAY_PATH_NORMAL_SLOT                         9
#define SRV_RAY_PATH_SHAPES_SLOT                         6
#define SRV_RAY_RECON_FRAME_SLOT                         14
#define SRV_RAY_RECON_HISTORY_SLOT                       29
#define SRV_RAY_RECON_SURFACE_SLOT                       15
#define SRV_RAY_REFLECTION_SLOT                          20
#define SRV_RAY_REFLECTION_ENVIRONMENT_SLOT              8
#define SRV_RAY_REFLECTION_ENV_TABLE_SLOT                9
#define SRV_RAY_REFLECTION_LIGHTS_SLOT                   4
#define SRV_RAY_REFLECTION_PROBE_SLOT                    11
#define SRV_RAY_REFLECTION_PROBE_OUTER_SLOT              12
#define SRV_RAY_REFLECTION_SHAPES_SLOT                   10
#define SRV_RAY_RESOLVE_RADIANCE_SLOT                    5
#define SRV_RAY_RESOLVE_SURFACE_SLOT                     6
#define SRV_RAY_SCENE_SLOT                               0
#define SRV_RAY_SPATIAL_FRAME_SLOT                       15
#define SRV_RAY_SPATIAL_HISTORY_SLOT                     14
#define SRV_RAY_SPATIAL_SURFACE_SLOT                     29
#define SRV_REFLECTION_RAW_SLOT                          5
#define SRV_REFLECTION_RAW_HALF_SLOT                     19
#define SRV_REFLECTION_RECON_ALBEDO_SLOT                 16
#define SRV_REFLECTION_RECON_HISTORY_SLOT                18
#define SRV_REFLECTION_RECON_NORMAL_SLOT                 17
#define SRV_REFLECTION_RECON_PREVIOUS_SURFACE_SLOT       15
#define SRV_REFLECTION_RECON_SURFACE_SLOT                14
#define SRV_REFLECTION_SPATIAL_HISTORY_SLOT              14
#define SRV_REFLECTION_SPATIAL_SURFACE_SLOT              15
#define TEX_SCENE_DEPTH_SLOT                             25
#define TEX_SCREEN_AO_SLOT                               23
#define SRV_SCREEN_REFLECTION_SLOT                       23
#define TEX_SHADOW_SLOT                                  8
#define SB_SKIN_BONES_SLOT                               15
#define SRV_SKIN_ROWS_SLOT                               29
#define SB_SKIN_SRC_VERTICES_SLOT                        14
#define TEX_SSAO_SLOT                                    9
#define TEX_SSR_SLOT                                     19
#define SRV_SSR_GBUFFER0_SLOT                            0
#define TEX_TAA_HISTORY_SLOT                             21
#define SRV_TAA_HISTORY_DEPTH_SLOT                       6
#define SRV_TAA_PHASE0_SLOT                              0
#define SRV_TAA_PHASE1_SLOT                              1
#define SRV_TAA_PHASE2_SLOT                              2
#define SRV_TAA_PHASE3_SLOT                              3
#define SRV_TAA_PHASE4_SLOT                              4
#define SRV_TAA_PHASE5_SLOT                              10
#define SRV_TAA_PHASE6_SLOT                              14
#define SRV_TAA_PHASE7_SLOT                              15
#define SRV_TAA_RADIANCE_SLOT                            8
#define SRV_TAA_REACTIVE_SLOT                            9
#define SRV_TAA_SURFACE_DEPTH_SLOT                       13
#define SRV_TAA_SURFACE_MATERIAL_SLOT                    11
#define SRV_TAA_SURFACE_NORMAL_SLOT                      12
#define SRV_TERRAIN_HIZ_SLOT                             28
#define SRV_TERRAIN_SHADOW_SLOT                          13
#define SRV_TERRAIN_SPLAT_INDICES_SLOT                   0
#define SRV_TERRAIN_SPLAT_WEIGHTS_SLOT                   1
#define SRV_UNDERWATER_VOLUME_SLOT                       21
#define SRV_VEGETATION_SLOTS_SLOT                        27
#define TEX_VELOCITY_SLOT                                26
#define TEX_VELOCITY_FIELD_SLOT                          26
#define TEX_VOLUMETRIC_SLOT                              20
#define SRV_VOLUMETRIC_RAW_SLOT                          5
#define SRV_VOLUME_MEDIUM_SLOT                           0
#define SRV_VOLUME_MEDIUM_UPLOAD_SLOT                    14
#define SRV_VOLUME_PUFFS_SLOT                            14
#define SRV_VOLUME_VELOCITY_SLOT                         1
#define SRV_VOLUME_VELOCITY_UPLOAD_SLOT                  15
#define SRV_WATER_DETAIL_NOISE_SLOT                      4
#define SRV_WATER_FOAM_SLOT                              3
#define SRV_WATER_INTERFACE_SLOT                         13
#define SRV_WATER_MEDIUM_ATLAS_SLOT                      15
#define SRV_WATER_MEDIUM_TABLE_SLOT                      14
#define SRV_WATER_PATTERN_SLOT                           0
#define SRV_WATER_RIPPLE_SLOT                            8
#define SRV_WATER_SCENE_COLOR_SLOT                       6
#define SRV_WATER_SCENE_DEPTH_SLOT                       5
#define SRV_WATER_SHADOW_SLOT                            9

/// @brief 書き込み先 UAV。異なるパスで同じ番号を再利用できる。
#define UAV_BAKE_PALETTE_SLOT                            2
#define UAV_BRDF_LUT_SLOT                                0
#define UAV_CLUSTER_LIGHTS_SLOT                          2
#define UAV_CONTACT_SHADOW_SLOT                          3
#define UAV_CULL_ARGUMENTS_SLOT                          3
#define UAV_CULL_VISIBLE_SLOT                            2
#define UAV_EXPOSURE_HISTOGRAM_SLOT                      2
#define UAV_EXPOSURE_RESULT_SLOT                         3
#define UAV_FLUID_ADVECT_SCALAR_FIELD_OUT_SLOT           0
#define UAV_FLUID_ADVECT_VELOCITY_VELOCITY_OUT_SLOT      0
#define UAV_FLUID_CLEAR_OUT0_SLOT                        0
#define UAV_FLUID_CLEAR_OUT1_SLOT                        1
#define UAV_FLUID_CONFINE_VELOCITY_OUT_SLOT              0
#define UAV_FLUID_CORRECT_FIELD_OUT_SLOT                 0
#define UAV_FLUID_CURL_CURL_OUT_SLOT                     0
#define UAV_FLUID_DIVERGENCE_DIVERGENCE_OUT_SLOT         0
#define UAV_FLUID_FORCES_VELOCITY_OUT_SLOT               0
#define UAV_FLUID_INJECT_EXPANSION_OUT_SLOT              2
#define UAV_FLUID_INJECT_FUEL_COLOR_OUT_SLOT             3
#define UAV_FLUID_INJECT_SCALARS_OUT_SLOT                0
#define UAV_FLUID_INJECT_VELOCITY_OUT_SLOT               1
#define UAV_FLUID_JACOBI_PRESSURE_OUT_SLOT               0
#define UAV_FLUID_OUTPUT_MEDIUM_OUT_SLOT                 0
#define UAV_FLUID_OUTPUT_VELOCITY_OUT_SLOT               1
#define UAV_FLUID_PROJECT_VELOCITY_OUT_SLOT              0
#define UAV_FLUID_SOLID_SOLID_OUT_SLOT                   0
#define UAV_FROXEL_INTEGRATED_SLOT                       1
#define UAV_FROXEL_SCATTER_SLOT                          0
#define UAV_GPU_PARTICLES_SLOT                           2
#define UAV_GPU_SORT_SLOT                                3
#define UAV_GTAO_BLUR_SLOT                               7
#define UAV_GTAO_RAW_SLOT                                6
#define UAV_HIZ_PYRAMID_SLOT                             2
#define UAV_IBL_FACE_SLOT                                0
#define UAV_LIGHT_PROBE_SH_SLOT                          0
#define UAV_LIGHT_PROBE_VISIBILITY_SLOT                  1
#define UAV_LIQUID_PREDICTION_SLOT                       3
#define UAV_LIQUID_RANGES_SLOT                           2
#define UAV_LIQUID_LAMBDA_SLOT                           2
#define UAV_LIQUID_DELTA_SLOT                            2
#define UAV_LIQUID_SORT_SLOT               3
#define UAV_LIQUID_STATE_SLOT                            2
#define UAV_MESHLET_DISPATCH_SLOT                        3
#define UAV_MESHLET_VISIBLE_SLOT                         2
#define UAV_MESH_OCCLUSION_ARGUMENTS_SLOT                2
#define UAV_MOTION_BLUR_SLOT                             5
#define UAV_OUTPUT_SLOT                                  0
#define UAV_OUTPUT2_SLOT                                 1
#define UAV_RAY_GAME_SURFACE_SLOT                        3
#define UAV_RAY_GAME_TRANSPORT_SLOT                      2
#define UAV_RAY_PATH_GEOMETRY_SLOT                       5
#define UAV_RAY_PATH_HISTORY_SLOT                        2
#define UAV_RAY_PATH_IDS_SLOT                            3
#define UAV_RAY_PATH_MATERIAL_SLOT                       4
#define UAV_RAY_PATH_SURFACE_SLOT                        1
#define UAV_RAY_RECON_HISTORY_SLOT                       2
#define UAV_RAY_REFLECTION_HALF_SLOT                     1
#define UAV_RAY_REFLECTION_METADATA_SLOT                 2
#define UAV_RAY_RESULT_SLOT                              0
#define UAV_REFLECTION_RECON_HISTORY_SLOT                2
#define UAV_SKINNED_VERTICES_SLOT                        4
#define UAV_SSR_SLOT                                     3
#define UAV_STRUCTURED_FIRST_SLOT                        2
#define UAV_VOLUMETRIC_SLOT                              4
#define UAV_VOLUME_MEDIUM_SLOT                           0
#define UAV_VOLUME_VELOCITY_SLOT                         1

/// @brief 頂点ステージの bindless 添字。異なるパスで同じ番号を再利用できる。
#define VS_SB_BUFFER0_SLOT                               1
#define VS_SB_BUFFER1_SLOT                               2
#define VS_SB_BUFFER_FIRST_SLOT                          1
#define VS_SB_INSTANCE_SLOT                              0
#define VS_SB_MESHLET_SELECTION_SLOT                     3

/// @brief サンプラー。異なるパスで同じ番号を再利用できる。
#define SAMPLER_COOKIE_SLOT                              5
#define SAMPLER_DEFAULT_SLOT                             0
#define SAMPLER_LINEAR_CLAMP_SLOT                        2
#define SAMPLER_POINT_CLAMP_SLOT                         3
#define SAMPLER_SHADOW_SLOT                              1
#define SAMPLER_SHADOW_PUNCTUAL_SLOT                     7
#define SAMPLER_UI_SLOT                                  5
#define SAMPLER_UI_POINT_SLOT                            6
#define SAMPLER_WRAP_ANISO4_SLOT                         8
#define SAMPLER_WRAP_LINEAR_SLOT                         4

#endif
