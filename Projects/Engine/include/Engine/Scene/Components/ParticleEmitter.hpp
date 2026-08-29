/// @file    ParticleEmitter.hpp
/// @brief   パーティクルの発生・寿命・見た目の設定一式。描画リソースは Renderer 側が持つ。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#pragma once
#include <Engine/Asset/ParticleMaterialSettings.hpp> // .mat の [particle] + Particle* 列挙
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Scene/Entity.hpp>
// ParticleCurve / ParticleGradient は Script からも宣言できるよう別ヘッダーに住む。
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {


// CS/VS 共通の GPU パーティクル 1 粒子レイアウト (96 bytes, 16-byte aligned)
// StructuredBuffer<GpuParticle> に格納し、CS が lifetime/age を更新、VS が位置を読む。
struct GpuParticle {
    math::Vector3 position;        // 12B
    float         size;            // 4B
    math::Vector3 velocity;        // 12B
    float         age;             // 4B
    math::Vector4 color;           // 16B
    float         lifetime;        // 4B
    float         rotation;        // 4B
    float         angularVelocity; // 4B
    float         spriteSeed;      // 4B
    math::Vector4 uvRect;          // 16B
    // 粒子ごとの色倍率 (colorVariation の結果)。
    // WHY: CS は毎フレーム色を CB の colorStart/End (または gradient) から作り直すため、
    //      スポーン時に配ったゆらぎがそのままでは翌フレームに消える。倍率として保持し、
    //      再計算した色へ毎フレーム掛け直すことで CPU 経路と同じ見た目になる。
    math::Vector3 colorScale;      // 12B
    float         colorScalePad;   // 4B
};

// CPU → CS へのスポーンリクエスト 1 件 (96 bytes, 16-byte aligned)
// DYNAMIC StructuredBuffer に毎フレーム書き込み、CS がリングバッファで配置する。
struct GpuSpawnEntry {
    math::Vector3 position;        // 12B
    float         lifetime;        // 4B
    math::Vector3 velocity;        // 12B
    float         size;            // 4B
    math::Vector4 colorStart;      // 16B
    // 粒子ごとの色ゆらぎ倍率 (xyz)。w は未使用。
    // CPU が求めた倍率をそのまま渡す。基準色との「比」から復元しようとすると、
    // 黒に近いチャンネルで暴れ、グラデーション使用時はそもそも成立しない。
    math::Vector4 colorScale;      // 16B
    math::Vector4 uvRect;          // 16B
    float         rotation;        // 4B
    float         angularVelocity; // 4B
    float         spriteSeed;      // 4B
    float         pad1;            // 4B
};

static_assert(sizeof(GpuParticle) == 96, "GpuParticle must match ParticleGpuSim.cs.hlsl (96 bytes)");
static_assert(sizeof(GpuSpawnEntry) == 96, "GpuSpawnEntry must match ParticleGpuSim.cs.hlsl (96 bytes)");

// 1 粒子が保持するトレイル履歴の最大点数。
// WHY: 固定長にして Particle を POD のまま保つ。粒子ごとに vector を持たせると
//      スポーン/消滅のたびにヒープ確保が走り、数千粒子では確保コストが支配的になる。
inline constexpr int kMaxParticleTrailPoints = 8;

struct Particle {
    math::Vector3 position;
    math::Vector3 velocity;
    math::Vector4 color;
    float         size;
    float         age;
    float         rotation = 0.0f;
    float         angularVelocity = 0.0f;
    float         lifetime = 1.0f;
    float         startSize = 1.0f;
    float         endSize = 0.0f;
    float         spriteSeed = 0.0f;
    math::Vector4 startColor = { 1, 1, 1, 1 };
    math::Vector4 endColor = { 1, 1, 1, 0 };
    // 発生時に配る色ゆらぎ倍率。毎フレーム作り直す色へ掛け直すため保持する。
    // WHY: グラデーション使用時は color が毎フレーム上書きされるので、
    //      スポーン時に色そのものへ焼き込むとゆらぎが翌フレームに消える。
    math::Vector3 colorScale = { 1.0f, 1.0f, 1.0f };
    math::Vector4 uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    math::Vector4 nextUvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    float spriteBlend = 0.0f;
    // トレイル履歴。[0] が最新で、後ろほど古い (＝尾の先端側)。
    // 位置はシミュレーション空間で持ち、描画時に粒子本体と同じ変換を通す。
    std::array<math::Vector3, kMaxParticleTrailPoints> trailPoints{};
    uint8_t trailCount = 0;
    float   trailSampleTimer = 0.0f;
};

// ParticleBurst — 再生時間上の繰り返しBurst設定。
struct ParticleBurst {
    float time = 0.0f;
    int count = 10;
    int cycles = 1;
    float interval = 0.1f;
    float probability = 1.0f;
};

// MeshShapeVertex — MeshSurface が静的頂点とスキンウェイトを共通形式で保持する。
// WHY: AssetManager 所有の Model を参照し続けず、スポーン時は必要な頂点情報だけを高速に抽選する。
struct MeshShapeVertex {
    math::Vector3 position;
    math::Vector3 normal;
    uint32_t boneIndices[4] = {};
    float boneWeights[4] = {};
    bool skinned = false;
};

// MeshShapeTriangle — MeshSurface の面積重み抽選 1 枚ぶん。頂点は meshShapeVertices への添字。
// 頂点を一様抽選すると «頂点密度» に比例して湧き、細かい部位からしか粒が出ない。
// 面積に比例させ、三角形内部を重心座標で取れば低ポリでも表面が埋まる。
// 重みはバインドポーズ面積で足りる (スキニングで伸縮しても抽選の比率はほぼ変わらない)。
struct MeshShapeTriangle {
    uint32_t indices[3] = {};
    // 先頭からこの三角形までの面積の総和。1 回の乱数を二分探索で引くための累積分布。
    float    cumulativeArea = 0.0f;
};

// materialPath 未設定の Emitter が使う既定 .mat。加算の丸い光。
// 何も無いと 1x1 白 (alpha=1) へ落ち、粒子が「不透明な四角」として描かれる。
// これが無いプロジェクトでは従来どおり白へ落ちる (存在しなくても壊れない)。
inline constexpr const char* PARTICLE_FALLBACK_MATERIAL =
    "Assets/Materials/Particles/ParticleFallback.mat";

/// ParticleEmitter のランタイム状態。シーンに保存しない一切をここへ集める。
///
/// WHY オーサリング設定と分けるか:
///   ParticleEmitter は «デザイナーが決める設定» と «実行中にしか意味の無い状態» が
///   同居していた。分かれていないと 3 つ困る。
///     1. Scene::CopyComponentsFrom はコンポーネント配列を値コピーするため、
///        複製した Emitter が GPU バッファハンドルを共有してしまう。
///     2. 「保存対象外」を人間がコメントで覚えるしかなく、Reflect / コーデックへ
///        誤って足しても気付けない。
///     3. 設定だけ読みたいシステムまでランタイム状態をキャッシュに載せる。
///
/// WHY コピーで引き継がないか:
///   GPU バッファ・粒子列・ロード済みパスは «その個体が今どうなっているか» であって
///   設定ではない。複製先が元のハンドルを共有すると、どちらかの破棄で他方が壊れる。
///   «コピーすると初期状態» という規則にすれば、複製のたびにリセットを書かなくて済む。
struct ParticleRuntime {
    std::vector<Particle> particles;
    // emitRate * dt の累積値。1.0 を超えるたびに 1 粒子を発生させる。
    // こうすることで低フレームレートでも発生数が dt に比例して安定する。
    float                 emitAccum = 0.0f;
    // randomSeed から初期化されるランタイム状態。シーン保存対象ではない。
    uint32_t              randomState = 1;
    float                 playTime = 0.0f;
    float                 delayTime = 0.0f;
    int                   burstPending = 0;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    std::string           loadedTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> motionVectorTexture;
    std::string           loadedMotionVectorTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> distortionTexture;
    std::string           loadedDistortionTexturePath;
    std::string           loadedMaterialPath; // materialPath の変更検出用。シーン保存対象外。

    // .mat が shader を指定していたときの描画シェーダー。無効なら組み込み Particle.hlsl。
    // 組み込みは「テクスチャを貼ったビルボード」しか出せないので、放電のように手続きで
    // 形を作る素材は差し替える。
    // 差し替えたシェーダーは Material/Effects/ParticleMaterial.hlsli を include し、
    // .mat には render_path = "particle" を書くこと。フリップブック・歪み・煙は自前で書く。
    renderer::ResourceHandle<renderer::ShaderTag> customShader;
    std::string           loadedShaderPath;   // customShader の変更検出用。シーン保存対象外。
    // カスタムシェーダーが宣言した MaterialConstants (b2) へ流す .mat の [params]。
    // 組み込みシェーダーは MaterialConstants を宣言しないので無効ハンドルのまま。
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialParamsCB;

    // .mat から解決した «見た目» 一式。シミュレーションも描画もこちらだけを見る。
    // settings はシーン保存対象なので書き戻さない («触っていないのに保存内容が変わる» /
    // «Inspector で変えても次のフレームで戻る» が起きる)。
    asset::ParticleMaterialSettings material;
    // .mat の blend_mode から解決した実効ブレンド。PSO 選択はこれを見る。
    ParticleBlendMode resolvedBlend = ParticleBlendMode::Additive;
    // albedo テクスチャが sRGB でエンコードされているか (.meta の srgb)。
    // WHY: 手描き素材は sRGB、ProceduralVFXTextures が焼くものはリニア。
    //      一律にリニア化すると後者が暗く沈む。素材ごとの実際の値に従う。
    bool                  textureIsSrgb = true;
    // .mat の [params] albedo。リニアへ変換済み。
    math::Vector4         materialTint = { 1.0f, 1.0f, 1.0f, 1.0f };
    // 黒体放射を焼き込んだ後の実効グラデーション。blackbodyEnabled が false なら
    // colorGradient のコピー。シミュレーションと GPU 定数バッファはこちらだけを見る。
    ParticleGradient      runtimeGradient;
    // RefreshRuntimeGradient の再計算判定に使う入力の写し。シーン保存対象外。
    bool                                                       runtimeGradientValid = false;
    bool                                                       runtimeGradientBlackbody = false;
    float                                                      runtimeGradientReference = 0.0f;
    float                                                      runtimeGradientIntensity = 0.0f;
    std::array<ParticleGradientKey, kMaxParticleCurveKeys>     runtimeGradientSourceKeys{};
    uint32_t                                                   runtimeGradientSourceCount = 0;
    std::array<ParticleCurveKey, kMaxParticleCurveKeys>        runtimeGradientTemperatureKeys{};
    uint32_t                                                   runtimeGradientTemperatureCount = 0;

    // GPU パーティクル実行時状態 (シーン保存不要、デバイスリセット時に再生成)
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuParticleBuffer; // RWStructuredBuffer: CS が更新
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSpawnBuffer;    // DYNAMIC SRV: CPU がスポーンデータを書く
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuEmitterCB;      // CS 用エミッター定数バッファ
    renderer::ResourceHandle<renderer::ConstantBufferTag>   renderCB;          // VS/PS 描画モード・Soft Particle
    // GPU ソート。sortMode != None のときだけ確保する。並べ替えるのは (キー, 粒子 index)
    // の対だけ (プール自体を動かすとリングバッファ位置が変わってスポーンが壊れる)。
    // 連続リボン (trailRibbon)。帯の頂点は毎フレーム CPU で作り直す (GPU では履歴を持てない)。
    // 頂点バッファは描画側の DynamicVertexBufferPool から借りる。帯はビューごとに形が
    // 変わるので、エミッターに 1 本持たせると後のビューが先のビューの Draw を書き替える。
    renderer::ResourceHandle<renderer::ConstantBufferTag>   trailRibbonCB;
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSortBuffer;     // RWStructuredBuffer<uint2>
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuSortCB;         // bitonic の (k, j) を段ごとに更新
    uint32_t gpuSortCapacity = 0;   // gpuSortBuffer の要素数 (2 のべき乗、maxParticles 以上)
    uint32_t gpuWriteHead    = 0;   // gpuSpawnBuffer の次書き込み位置 (リングバッファインデックス)
    uint32_t gpuSpawnCount   = 0;   // 今フレームのスポーン数
    bool     gpuInitialized  = false;
    uint32_t gpuCapacity = 0;
    bool     gpuClearPending = false;
    uint64_t gpuResetVersion = 0;   // 最後に確認した ResourceManager::GetResetVersion()
    uint64_t lastGpuSimulationFrame = UINT64_MAX;
    uint64_t lastCpuSimulationFrame = UINT64_MAX;
    uint64_t lastPlaybackFrame = UINT64_MAX;
    bool emitThisFrame = false;
    bool prewarmed = false;
    math::Vector3 boundsCenter = {};
    float boundsRadius = 0.0f;
    float lodRateScale = 1.0f;
    int visibleParticleCount = 0;
    bool isCulledThisFrame = false;
    int prewarmSpawnPending = 0;
    bool hasLastEmitterPosition = false;
    math::Vector3 lastEmitterPosition = {};
    // エミッター自身のワールド速度 [m/s]。inheritVelocity がスポーン時に参照する。
    // ParticleSimulationSystem が lastEmitterPosition を更新するのと同じ場所で毎フレーム求める。
    math::Vector3 emitterVelocity = {};
    float distanceEmitAccum = 0.0f;
    std::vector<int> burstCyclesFired;

    // MeshSurface Shapeのランタイムキャッシュ。位置・法線と4ボーンウェイトだけを複製する。
    std::vector<MeshShapeVertex>   meshShapeVertices;
    // 面積の累積分布付き三角形列。インデックスを持たないメッシュでは空のままで、
    // その場合は頂点の一様抽選へ縮退する。
    std::vector<MeshShapeTriangle> meshShapeTriangles;
    std::string loadedMeshShapePath;
    int         loadedMeshShapeIndex = -2;

    // ── エディタープレビュー制御 (VFX Editor 用ランタイム状態。シーン保存対象外) ──
    // WHY: VFX Editor の再生速度・一時停止をシミュレーション dt へ注入するための口。
    //      パネルが毎フレーム editorTimeScale と editorTimeScaleFrame を書き込み、
    //      書き込みが途絶えたら自動で通常速度へ戻る (パネルを閉じても凍結が残らないフェイルセーフ)。
    float    editorTimeScale      = 1.0f;
    uint64_t editorTimeScaleFrame = 0;     // editorTimeScale を最後に書き込んだ Time::frameCount
    // タイムラインスクラブ要求 [秒]。>=0 のとき ParticleSimulationSystem が消費し、
    // randomSeed から決定論的にその時刻まで再シミュレートする。負値 = 要求なし。
    float    editorScrubTime      = -1.0f;

    // 当たり・死亡の「今フレームぶん」の計数。VFX Graph の OnCollision / OnDeath が消費する。
    int collisionCountThisFrame = 0;
    int deathCountThisFrame = 0; // VFX GraphのOnDeath eventがフレーム単位で消費する。
    ParticleRuntime() = default;
    ParticleRuntime(ParticleRuntime&&) noexcept = default;
    ParticleRuntime& operator=(ParticleRuntime&&) noexcept = default;

    // コピーは「初期状態」を作る。GPU ハンドルや粒子列を引き継がせないための規則。
    ParticleRuntime(const ParticleRuntime&) {}
    ParticleRuntime& operator=(const ParticleRuntime&)
    {
        *this = ParticleRuntime{};   // 一時からの move 代入。常に初期状態へ戻す
        return *this;
    }
};


/// エミッターのオーサリング設定。**GameObject に依存しない値の塊**。
///
/// WHY コンポーネントから分けるか:
///   ParticleEmitter は 2 つの役を兼ねていた。(A) VFXGraphNode が値で埋め込み
///   .particle / .vfx へ保存する «オーサリングの塊»、(B) GameObject に付いて
///   simulate される «シーンコンポーネント»。A には GameObject が無いので、
///   両者を同じ型にしていると «設定だけ写す» 操作が書けない。実際 VFXGraphSystem は
///   それを書けず、TOML へ書き出して即座に読み戻すという往復で代用していた。
///   型を分ければ `emitter.settings = node.particle;` の 1 行になる。
///
/// WHY 直列化の正本をここへ寄せるか:
///   保存対象は「この構造体の中身」と一致する。ランタイム状態が混ざっていたときは
///   codec と Reflect() を人手で二重管理するしかなく、実際に 29 フィールドがズレて
///   AI バス (Reflect 経由) から見えなくなっていた。
struct ParticleEmitterSettings {

    math::Vector3 emitPosition   = {};
    math::Vector3 emitVelocity   = { 0.0f, 4.0f, 0.0f };
    // 初速のばらつき [m/s]。emitVelocity の 3 軸へ等方に ±velocitySpread を加える。
    // Sphere / Cone 形状が持つ方向成分 (shapeVelocity) とは別枠で、こちらは純粋な揺らぎ。
    float         velocitySpread = 1.5f;
    math::Vector4 colorStart     = { 1.0f, 0.7f, 0.2f, 1.0f };
    math::Vector4 colorEnd       = { 1.0f, 0.1f, 0.0f, 0.0f };
    float         sizeStart      = 0.4f;
    float         sizeEnd        = 0.05f;
    // ビルボードの縦横比。size に対する軸ごとの倍率で、xy のみ使う (z は Mesh Particle 用)。
    // エミッター単位の値なので per-particle ではなく定数バッファへ載せる。
    math::Vector3 sizeAxisScale  = { 1.0f, 1.0f, 1.0f };
    float         lifetime       = 2.0f;
    float         lifetimeRandom = 0.0f;
    float         emitRate       = 30.0f;
    int           maxParticles   = 300;
    // WHY: 固定重力では炎・煙・火花の挙動を作り分けられないため、エミッター単位で加速度を持つ。
    math::Vector3 gravity        = { 0.0f, -5.0f, 0.0f };
    // WHY: std::rand() のグローバル状態を避け、エミッター単位で再現可能な分布にする。
    uint32_t      randomSeed     = 1;
    bool          enabled        = true;

    // 再生状態。enabled は Component の有効/無効、playing はエフェクト再生を表す。
    bool  playing     = true;
    bool  loop        = true;
    float duration    = 5.0f;
    float startDelay  = 0.0f;
    bool  clearOnStop = false;

    ParticleEmitterShape shape = ParticleEmitterShape::Point;
    float         sphereRadius = 1.0f;
    float         coneAngleDegrees = 25.0f;
    float         coneRadius = 1.0f;
    math::Vector3 boxExtents = { 1.0f, 1.0f, 1.0f };
    // FBX / Model の頂点群を発生位置として使う。meshIndex < 0 なら全サブメッシュを結合する。
    // WHY: CPU/GPUの両モードで同じスポーンバッファを使い、モデル形状Particleを同じ見た目にする。
    std::string meshShapePath;
    int         meshShapeIndex = -1;
    float       meshShapeScale = 1.0f;
    // 同じ GameObject の AnimatorComponent が持つ現在のボーン行列で発生点を変形する。
    bool        meshShapeFollowSkinnedAnimation = false;
    // 表面法線方向へ与える初速 [m/s]。0 なら emitVelocity だけで飛ぶ。
    // 表面から «にじみ出る» 崩壊は、法線が初速に入って初めて成立する。
    // 負値は «表面へ吸い込む» 収束エフェクトになる。
    float       meshShapeNormalVelocity = 0.0f;

    ParticleSortMode  sortMode  = ParticleSortMode::None;
    // エミッター間の描画順。小さいほど先に描かれる (＝奥に見える)。
    // sortMode は 1 エミッター内しか並べ替えないので、別エミッターが重なる構成では
    // GameObject の並び順で前後が決まってしまう。同値ならカメラから遠い順。
    int renderPriority = 0;
    ParticleSimulationMode simulationMode = ParticleSimulationMode::Cpu;
    ParticleSimulationSpace simulationSpace = ParticleSimulationSpace::World;
    ParticleRenderMode renderMode = ParticleRenderMode::Billboard;
    float stretchedVelocityScale = 0.1f;
    float stretchedLengthScale = 1.0f;

    // CollisionはCPUで自作Physics WorldをQueryする。GPU指定時は正確性優先でCPUへ縮退する。

    // 見た目の正本となる .mat アセットへの参照。ブレンド・アルベド・フリップブック・
    // 歪み・煙・自己影・明るさはすべてここから来る (解決結果は runtime.material)。
    // 同じ素材を複数のエミッターで共有するため .mat 側に持つ。
    // 空のときは PARTICLE_FALLBACK_MATERIAL が使われる (白い矩形にはならない)。
    std::string materialPath;
    // 空でない場合はbillboardの代わりに静的Meshを各CPU粒子のTRSで描画する。
    std::string meshParticlePath;
    // 粒子ごとの色ゆらぎ [0,1]。発生時に RGB を各チャンネル独立で ±colorVariation 倍する。
    // 完全に同色だと群れが一枚のベタ塗りに見える。チャンネル独立にすると明度差と
    // 軽い色相差が同時に出て厚みが出る。
    float colorVariation = 0.0f;
    float sizeCurvePower = 1.0f;
    float colorCurvePower = 1.0f;
    float velocityDamping = 0.0f;
    float angularVelocityMin = 0.0f;
    float angularVelocityMax = 0.0f;
    bool useSizeCurve = false;
    ParticleCurve sizeCurve;
    bool useVelocityCurve = false;
    ParticleCurve velocityCurve;
    bool useColorGradient = false;
    ParticleGradient colorGradient;

    // 角速度に掛ける時間倍率。定数の angularVelocity だけでは
    // 「勢いよく回り始めて減速する」火の粉・破片の動きが作れない。
    bool useRotationCurve = false;
    ParticleCurve rotationCurve;
    // velocityDamping に掛ける時間倍率。噴き出し直後は素直に飛び、
    // 後半で急に空気抵抗が効く、といった減衰の作り分けに使う。
    bool useDragCurve = false;
    ParticleCurve dragCurve;

    // ── 発生量の時間変化 ──
    // emitRate に掛ける倍率。エミッターの再生時刻を duration で正規化した 0..1 で引く。
    // burst を刻んで近似すると粒の湧き方が段になって «脈打つ» ように見える。
    // loop するエミッターでは playTime を duration で割った余りで引く。
    bool useEmitRateCurve = false;
    ParticleCurve emitRateCurve;

    // ── 速度による見た目 ──
    // 粒の «速さ» で大きさと色を変える。寿命ベースのカーブとは別の軸。
    // 火花は «速いものほど明るく長い» のであって «出てすぐが明るい» のではないので、
    // 寿命だけを見ると跳ね返って減速した粒が «まだ若いから明るい» まま残る。
    // 正規化: speed / speedRange を 0..1 にクランプしてカーブを引く。
    float speedRange = 10.0f;
    bool useSpeedSizeCurve = false;
    ParticleCurve speedSizeCurve;         // size への倍率
    bool useSpeedColorGradient = false;
    ParticleGradient speedColorGradient;  // 寿命の色へ乗算する

    // ── 速度モジュール (エミッター原点まわりの周回・放射) ──
    // WHY: 重力とノイズだけでは「渦を巻きながら広がる」魔法陣・竜巻・吸い込みが作れない。
    //      ForceField はシーン全体の場だが、こちらはエミッターに追従する固有の運動として効く。
    math::Vector3 orbitalAxis = { 0.0f, 1.0f, 0.0f }; // 周回の回転軸 (正規化して使う)
    float orbitalVelocity = 0.0f;  // 軸まわりの接線加速度 [m/s^2]
    float radialVelocity  = 0.0f;  // 原点から外向きの加速度 [m/s^2]。負で吸い込み
    // 発生時にエミッター自身の移動速度を初速へ加算する割合 [0,1]。
    // 移動する剣・ロケットから出る火花が置き去りにならず、引きずられて見えるようになる。
    float inheritVelocity = 0.0f;

    // Emission拡張: 移動距離、Prewarm、時刻指定Burst。
    float rateOverDistance = 0.0f;
    bool prewarm = false;
    std::vector<ParticleBurst> bursts;

    // 履歴点へビルボードを並べるのではなく、連続した 1 枚の帯として描く。
    // ビルボード方式は太くすると粒の連なりが露見するので、幅のある帯が主役の表現
    // (剣閃・魔法の軌跡) には使えない。有効時は Trail ノードと同じリボン生成を通す。
    // 帯は 1 エミッターを 1 DrawCall で描くので、色は colorStart / colorEnd を長さ方向へ配る
    // (粒子ごとの色ゆらぎを尾へ乗せたいならビルボード方式のまま)。
    // SubEmitterはGameObject名で参照し、各イベントで対象EmitterへBurstを積む。

    // Culling/LOD — 粒子の現在Boundsを使い、遠距離では発生数と描画数を段階的に削減する。
    bool cullingEnabled = true;
    float cullingBoundsPadding = 0.25f;
    bool lodEnabled = true;
    float lodNearDistance = 12.0f;
    float lodFarDistance = 40.0f;
    float lodNearRateScale = 1.0f;
    float lodFarRateScale = 0.25f;
    float screenCoverageThreshold = 0.0f;
    bool pauseWhenCulled = false;

    // ── ノイズモジュール (乱流ベクトルフィールド) ──
    // カールノイズ (発散ゼロのベクトル場) を粒子速度へ加算する。炎の揺らぎ・煙の乱れ用。
    // WHY: ParticleForceField(Turbulence) はシーン全体の場だが、こちらはエミッター固有の
    //      揺らぎとして粒子ごとに常時作用させたいケース (Unity の Noise モジュール相当) に使う。
    float noiseStrength  = 0.0f;  // 加速度の大きさ [m/s^2]。0 で無効
    float noiseFrequency = 0.5f;  // ノイズ格子の空間周波数 [1/m]
    float noiseSpeed     = 1.0f;  // 時間スクロール速度

    // シーン内の ParticleForceField から力を受けるか。
    // WHY: UI 演出用パーティクルなど、環境の風に反応させたくないエミッターを除外できるようにする。
    bool receiveForceFields = true;

    /// 受け取る力場を選ぶビットマスク。ParticleForceField::channels と 1 ビットでも
    /// 重なった力場だけがこのエミッターに作用する。receiveForceFields が false なら無関係。
    /// 既定は全ビット ON で、従来どおり全力場を受け取る。
    uint32_t forceFieldChannels = 0xFFFFFFFFu;



    // ── 任意機能 (既定のまま使わないことが多い) ──────────────────────────
    ParticleCollisionMode collisionMode = ParticleCollisionMode::None;
    ParticleCollisionResponse collisionResponse = ParticleCollisionResponse::Bounce;
    float collisionRadius = 0.05f;
    float collisionBounciness = 0.5f;
    float collisionDamping = 0.0f;
    float collisionPlaneY = 0.0f;
    // ── 黒体放射 (色温度オーサリング) ──
    // 有効にすると colorGradient の RGB を温度カーブから毎フレーム作り直す
    // (アルファはグラデーション側の値をそのまま使う)。
    // 炎・爆発の色はすす粒子の温度による黒体放射で決まるので、任意の RGB を並べても
    // 炎に見えない。輻射輝度は T^4 に比例し、根元と先端の差は色差ではなく輝度差として出る。
    bool  blackbodyEnabled = false;
    // 寿命 [0,1] → 色温度 [K]。既定は焚き火の実測域 (根元 1900K → 先端 1100K)。
    // WHY: ParticleCurve の既定キーは 0→1 で、そのまま温度として使うと 1K = 真っ黒になる。
    //      有効化した瞬間に炎らしい値が出ないと、機能があること自体に気付けない。
    ParticleCurve temperatureCurve{
        {{ {0.0f, 1900.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f},
           {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f} }},
        2, ParticleCurveInterpolation::Linear };
    float blackbodyReferenceTemperature = 1800.0f; // ここで intensity 倍の明るさになる
    float blackbodyIntensity = 1.0f;
    // ── per-particle Trail ──
    // 粒子 1 つ 1 つに尾を付ける。火の粉・魔法の軌跡のように「粒が線を引く」表現用。
    // 実装は履歴点へビルボードを連ねる方式で、専用の ribbon シェーダーは持たない。
    // 既存の描画 (シェーダー・PSO・テクスチャ・ブレンド) をそのまま使えるので素材が揃う。
    // 太い帯が要るなら Trail ノードを使うこと。
    // GPU シミュレーションでは履歴を保持できないため、有効時は CPU へ縮退する。
    bool  trailEnabled = false;
    int   trailPointCount = 6;          // 使用する履歴点数 [1, kMaxParticleTrailPoints]
    float trailSampleInterval = 0.03f;  // 履歴を刻む間隔 [秒]。短いほど滑らか
    float trailWidthScale = 0.6f;       // 尾の先端 (最古) 側のサイズ倍率
    float trailAlphaScale = 0.5f;       // 尾の先端側の不透明度倍率
    math::Vector4 trailColorTint = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  trailRibbon = false;
    // 帯の幅 [m]。0 以下なら粒子サイズをそのまま使う。
    // WHY: 帯は粒子サイズと独立に太さを決めたいことが多い (小さな火の粉が太い軌跡を引く等)。
    float trailRibbonWidth = 0.0f;
    int subEmitterBurstCount = 1;
    // 名前引きの探索範囲を限定するルート GameObject。INVALID でシーン全体。
    // 同じ .vfx を複数配置すると同名の GO が並ぶので、シーン全体で引くと隣のインスタンスを
    // 吹かせてしまう。VFXGraphSystem がここへ owner を入れて参照をエフェクト内へ閉じる。
    // ランタイム専用。シーン保存対象ではない。
    EntityID subEmitterScopeRoot = EntityID::INVALID;
    std::string birthSubEmitter;
    std::string deathSubEmitter;
    std::string collisionSubEmitter;

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("emitPosition", emitPosition);
        r.Field("emitVelocity", emitVelocity);
        r.Field("velocitySpread", velocitySpread);
        r.ColorField("colorStart", colorStart);
        r.ColorField("colorEnd", colorEnd);
        r.Field("sizeStart", sizeStart);
        r.Field("sizeEnd", sizeEnd);
        r.Field("lifetime", lifetime);
        r.Field("lifetimeRandom", lifetimeRandom);
        r.Field("emitRate", emitRate);
        r.Field("maxParticles", maxParticles);
        r.Field("gravity", gravity);
        r.Field("playing", playing);
        r.Field("loop", loop);
        r.Field("duration", duration);
        r.Field("startDelay", startDelay);
        r.Field("clearOnStop", clearOnStop);
        // IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        int seed = static_cast<int>(randomSeed);
        r.Field("randomSeed", seed);
        randomSeed = static_cast<uint32_t>(seed < 1 ? 1 : seed);
        r.Field("materialPath", materialPath);
        r.Field("meshParticlePath", meshParticlePath);
        int shapeValue = static_cast<int>(shape);
        r.Field("shape", shapeValue);
        shapeValue = shapeValue < 0 ? 0 : (shapeValue > 4 ? 4 : shapeValue);
        shape = static_cast<ParticleEmitterShape>(shapeValue);
        r.Field("sphereRadius", sphereRadius);
        r.Field("coneAngleDegrees", coneAngleDegrees);
        r.Field("coneRadius", coneRadius);
        r.Field("boxExtents", boxExtents);
        r.Field("meshShapePath", meshShapePath);
        r.Field("meshShapeIndex", meshShapeIndex);
        r.Field("meshShapeScale", meshShapeScale);
        r.Field("meshShapeFollowSkinnedAnimation", meshShapeFollowSkinnedAnimation);
        r.Field("meshShapeNormalVelocity", meshShapeNormalVelocity);

        int sortValue = static_cast<int>(sortMode);
        r.Field("sortMode", sortValue);
        sortValue = sortValue < 0 ? 0 : (sortValue > 1 ? 1 : sortValue);
        sortMode = static_cast<ParticleSortMode>(sortValue);

        int simValue = static_cast<int>(simulationMode);
        r.Field("simulationMode", simValue);
        simValue = simValue < 0 ? 0 : (simValue > 1 ? 1 : simValue);
        simulationMode = static_cast<ParticleSimulationMode>(simValue);

        int simulationSpaceValue = static_cast<int>(simulationSpace);
        r.Field("simulationSpace", simulationSpaceValue);
        simulationSpace = static_cast<ParticleSimulationSpace>(simulationSpaceValue < 0 ? 0 : (simulationSpaceValue > 1 ? 1 : simulationSpaceValue));
        int renderModeValue = static_cast<int>(renderMode);
        r.Field("renderMode", renderModeValue);
        renderMode = static_cast<ParticleRenderMode>(renderModeValue < 0 ? 0 : (renderModeValue > 3 ? 3 : renderModeValue));
        r.Field("stretchedVelocityScale", stretchedVelocityScale);
        r.Field("stretchedLengthScale", stretchedLengthScale);
        r.Field("sizeCurvePower", sizeCurvePower);
        r.Field("colorCurvePower", colorCurvePower);
        r.Field("velocityDamping", velocityDamping);
        r.Field("angularVelocityMin", angularVelocityMin);
        r.Field("angularVelocityMax", angularVelocityMax);
        r.Field("useSizeCurve", useSizeCurve);
        r.Field("useVelocityCurve", useVelocityCurve);
        r.Field("useColorGradient", useColorGradient);
        r.Field("rateOverDistance", rateOverDistance);
        r.Field("prewarm", prewarm);
        int gradientColorSpaceValue = static_cast<int>(colorGradient.colorSpace);
        r.Field("gradientColorSpace", gradientColorSpaceValue);
        colorGradient.colorSpace = static_cast<ParticleColorSpace>(
            gradientColorSpaceValue < 0 ? 0 : (gradientColorSpaceValue > 2 ? 2 : gradientColorSpaceValue));
        r.Field("cullingEnabled", cullingEnabled);
        r.Field("cullingBoundsPadding", cullingBoundsPadding);
        r.Field("lodEnabled", lodEnabled);
        r.Field("lodNearDistance", lodNearDistance);
        r.Field("lodFarDistance", lodFarDistance);
        r.Field("lodNearRateScale", lodNearRateScale);
        r.Field("lodFarRateScale", lodFarRateScale);
        r.Field("screenCoverageThreshold", screenCoverageThreshold);
        r.Field("pauseWhenCulled", pauseWhenCulled);
        r.Field("noiseStrength", noiseStrength);
        r.Field("noiseFrequency", noiseFrequency);
        r.Field("noiseSpeed", noiseSpeed);
        r.Field("receiveForceFields", receiveForceFields);
        // IReflector は符号なし整数を扱わない。ビットパターンは int 経由でも保たれる。
        int channelsValue = static_cast<int>(forceFieldChannels);
        r.Field("forceFieldChannels", channelsValue);
        forceFieldChannels = static_cast<uint32_t>(channelsValue);
    }
};

/// runtimeGradient を作り直す。入力が変わっていなければ何もしないので、どこから呼んでもよい。
/// @note 黒体モードでは各キーの時刻で温度カーブを引き、色温度 → リニア RGB → オーサリング空間
///       (sRGB) へ戻して格納する。オーサリング空間で持つのは、CPU 経路も GPU 経路も
///       「補間 → リニア化」という同じ順序を通すため。ここだけ別空間にすると片方が破綻する。
// WHY 自由関数か: 焼き込みは settings を読んで runtime へ書くだけの純粋な変換で、エミッター実体を
//      必要としない。実体を持たない VFX グラフのノードでも、Inspector が焼き込み後の色を出せる。
inline void RefreshParticleRuntimeGradient(const ParticleEmitterSettings& settings, ParticleRuntime& runtime)
{
    // 黒体の焼き込みはキー 1 点あたり可視域 81 サンプルの積分になる。粒子ごとの
    // 更新から間接的に呼ばれても潰れないよう、入力が変わったときだけ作り直す。
    if (runtime.runtimeGradientValid
        && !settings.blackbodyEnabled == !runtime.runtimeGradientBlackbody
        && runtime.runtimeGradientReference == settings.blackbodyReferenceTemperature
        && runtime.runtimeGradientIntensity == settings.blackbodyIntensity
        && runtime.runtimeGradientSourceKeys == settings.colorGradient.keys
        && runtime.runtimeGradientSourceCount == settings.colorGradient.keyCount
        && runtime.runtimeGradientTemperatureKeys == settings.temperatureCurve.keys
        && runtime.runtimeGradientTemperatureCount == settings.temperatureCurve.keyCount) {
        return;
    }
    runtime.runtimeGradientValid = true;
    runtime.runtimeGradientBlackbody = settings.blackbodyEnabled;
    runtime.runtimeGradientReference = settings.blackbodyReferenceTemperature;
    runtime.runtimeGradientIntensity = settings.blackbodyIntensity;
    runtime.runtimeGradientSourceKeys = settings.colorGradient.keys;
    runtime.runtimeGradientSourceCount = settings.colorGradient.keyCount;
    runtime.runtimeGradientTemperatureKeys = settings.temperatureCurve.keys;
    runtime.runtimeGradientTemperatureCount = settings.temperatureCurve.keyCount;

    runtime.runtimeGradient = settings.colorGradient;
    if (!settings.blackbodyEnabled) return;
    const uint32_t count = (std::min)(runtime.runtimeGradient.keyCount,
                                      static_cast<uint32_t>(runtime.runtimeGradient.keys.size()));
    for (uint32_t index = 0; index < count; ++index) {
        const float kelvin = settings.temperatureCurve.Evaluate(runtime.runtimeGradient.keys[index].time);
        const math::Vector3 linear = ParticleBlackbodyLinear(
            kelvin, settings.blackbodyReferenceTemperature, settings.blackbodyIntensity);
        runtime.runtimeGradient.keys[index].color = ParticleLinearToSrgb(
            { linear.x, linear.y, linear.z, runtime.runtimeGradient.keys[index].color.w });
    }
}

struct ParticleEmitter {
    /// オーサリング設定。VFXGraphNode などが持つ塊と同じ型。
    ParticleEmitterSettings settings;
    /// 実行中にしか意味を持たない状態。シーンにも .particle にも保存しない。
    ParticleRuntime runtime;

    void RefreshRuntimeGradient() { RefreshParticleRuntimeGradient(settings, runtime); }

    // 実効プレビュー速度。エディターからの書き込みが 2 フレーム以上途絶えていたら 1.0 に戻す。
    // WHY: ゲーム実行時 (エディターなし) は書き込みが存在しないため常に 1.0 になり、影響しない。
    float GetEditorTimeScale(uint64_t currentFrame) const
    {
        return (runtime.editorTimeScaleFrame + 2 >= currentFrame) ? runtime.editorTimeScale : 1.0f;
    }

    // 再生状態と粒子を先頭へ巻き戻す。Inspector / VFX Editor の Restart とスクラブ前処理が共用する。
    // WHY: リセットすべきランタイム状態が多く、呼び出し側ごとに列挙すると漏れが出るため一箇所に集約する。
    void ResetPlayback()
    {
        settings.playing                = true;
        runtime.playTime               = 0.0f;
        runtime.delayTime              = 0.0f;
        runtime.emitAccum              = 0.0f;
        runtime.burstPending           = 0;
        runtime.burstCyclesFired.clear();
        runtime.prewarmed              = false;
        runtime.prewarmSpawnPending    = 0;
        runtime.hasLastEmitterPosition = false;
        runtime.emitterVelocity        = {};
        runtime.distanceEmitAccum      = 0.0f;
        runtime.randomState            = settings.randomSeed != 0 ? settings.randomSeed : 1;
        runtime.particles.clear();
        runtime.gpuClearPending        = true;
        // ロード直後やスクラブ開始時に、再生が 1 フレームも進まないまま粒子色を
        // 引かれることがある。実効グラデーションを先に用意しておく。
        RefreshRuntimeGradient();
    }

    // Inspector / Script / Operator が共有する再生制御。
    // WHY: UI と ScriptProxy がそれぞれランタイム状態を列挙すると、Clear や Restart の
    //      対象漏れが経路ごとに発生する。エミッター自身に責務を集め、AI も同じ挙動を使う。
    void Play(bool restart = false)
    {
        if (restart) ResetPlayback();
        else         settings.playing = true;
    }

    void Pause() { settings.playing = false; }

    void Stop(bool clear = false)
    {
        settings.playing      = false;
        runtime.emitAccum     = 0.0f;
        runtime.burstPending  = 0;
        if (clear) ClearParticles();
    }

    void Burst(int count)
    {
        if (count > 0) runtime.burstPending += count;
    }

    void ClearParticles()
    {
        runtime.particles.clear();
        runtime.emitAccum = 0.0f;
        runtime.burstPending = 0;
        runtime.playTime = 0.0f;
        runtime.delayTime = 0.0f;
        runtime.gpuClearPending = true;
        runtime.gpuWriteHead = 0;
        runtime.gpuSpawnCount = 0;
        runtime.collisionCountThisFrame = 0;
        runtime.prewarmSpawnPending = 0;
    }

    const char* GetTypeName() const { return "Particle Emitter"; }

    /// 設定へ転送する。保存対象は settings の中身と一致するので、正本は 1 つで済む。
    void Reflect(IReflector& r)
    {
        const uint32_t seedBefore = settings.randomSeed;
        settings.Reflect(r);
        // 種を編集したら乱数列も追従させる。Reflect は Inspector / AI バスの入口なので、
        // ここで揃えないと「種を変えたのに絵が変わらない」になる。
        if (settings.randomSeed != seedBefore) runtime.randomState = settings.randomSeed;
    }
};

} // namespace fbzz::scene
