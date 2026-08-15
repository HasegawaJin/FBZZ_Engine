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
#define TEX_DECAL_MASK   t13 // デカール受信除外マスク (bit3 有効時のみバインド)
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
// ComputeCall::srvBuffers[0..1] が t14/t15 へ束縛される。GPU パーティクルの
// SB_GPU_PARTICLES / SB_GPU_SORT と同じスロットだが、別ディスパッチなので競合しない。
#define SB_SKIN_SRC_VERTICES t14 // StructuredBuffer<SkinSrcVertex>
#define SB_SKIN_BONES        t15 // StructuredBuffer<float4x4> (ボーンパレット)

// ---- Sampler ---------------------------------------------------------
#define SAMPLER_DEFAULT      s0
#define SAMPLER_SHADOW       s1   // SamplerComparisonState (PCF 用)
#define SAMPLER_LINEAR_CLAMP s2   // Linear clamp (IBL BRDF LUT / 3D LUT 用)
#define SAMPLER_POINT_CLAMP  s3   // Point clamp  (TAA 再投影ルックアップ用)
#define SAMPLER_WRAP_LINEAR  s4   // Linear wrap  (ボリューメトリック雲のタイラブル 3D ノイズ用)

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
#define TEX_CONTACT_SHADOW   t24  // コンタクトシャドウマスク
#define TEX_SCENE_DEPTH      t25  // Forward 不透明物を含む最終シーン深度 (SSR 遮蔽用)
#define TEX_CLOUD_SHAPE      t26  // ボリューメトリック雲 Shape 3D ノイズ (128³ Perlin-Worley)
#define TEX_CLOUD_DETAIL     t27  // ボリューメトリック雲 Detail 3D ノイズ (32³ Worley 高周波)

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

// ---- Clustered Lighting (Forward+ / Deferred+) ------------------------
#define CB_CLUSTER           b9   // ClusterConstants (グリッド寸法 / Z スライス係数 / ライト数)

// ピクセルシェーダーが読むライトデータ。DrawCall::psBuffers[0..1] がここへ束縛される。
// LAYOUT: Engine/Renderer/DrawCall.hpp の kPsBufferBaseSlot (=29) と一致させること。
// WHY t14/t15 を使わないか: あちらは両バックエンドとも頂点シェーダー専用スロットで、
//     PS からは読めない (DX11 は VSSetShaderResources、DX12 の root param 15 は VERTEX 可視)。
#define SB_PUNCTUAL_LIGHTS   t29  // StructuredBuffer<PunctualLight>  点光源+スポットを統合した配列
#define SB_CLUSTER_INDICES   t30  // StructuredBuffer<uint>  [cluster*stride] = 個数, 続けてライト番号

// カリング CS の入力。ComputeCall::srvBuffers[0] が t14 へ束縛される。
// GPU パーティクル / スキニングと同じスロットだが、別ディスパッチなので競合しない。
#define SB_CLUSTER_LIGHTS_CS t14  // StructuredBuffer<PunctualLight>

// カリング CS の出力。ComputeCall::uavBuffers[0] が u2 へ束縛される。
// WHY UAV が 1 本で足りるか: 1 スレッド = 1 クラスタにするとカウンタがスレッド内で完結するため、
//     InterlockedAdd も offset テーブルも不要になり、固定ストライドで直接書ける。
//     おかげで UAV_SSR / UAV_CONTACT_SHADOW / UAV_GPU_SORT が密集する u3 を避けられる。
#define UAV_CLUSTER_LIGHTS   u2   // RWStructuredBuffer<uint> (UAV_GPU_PARTICLES と時分割で再利用)

#endif // BINDING_HLSLI
