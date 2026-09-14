/// @file    ParticleEmitter.hpp
/// @brief   パーティクルの発生・寿命・見た目の設定一式。描画リソースは Renderer 側が持つ。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#pragma once
#include <Engine/Asset/ParticleMaterialSettings.hpp> // .mat の [particle] + Particle* 列挙
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Scene/Components/ForceField.hpp>
#include <Engine/Scene/Entity.hpp>
// ParticleCurve / ParticleGradient は Script からも宣言できるよう別ヘッダーに住む。
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
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
    /// 次のコマへの補間率 (Frame Blending)。0 なら nextUvRect は読まれない。
    float         spriteBlend;     // 4B
    /// 次のコマの UV 矩形。CS が EvaluateFlipbookFrame と同じ規則で書く。
    math::Vector4 nextUvRect;      // 16B
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

static_assert(sizeof(GpuParticle) == 112, "GpuParticle must match ParticleGpuSim.cs.hlsl (112 bytes)");
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

/// 発火元から注入されたスポーン 1 件。SubEmitter が «どこで・どう動いていたか» を渡す口。
///
/// WHY 個数だけでは足りないか: burstPending は «何個出すか» しか運べないため、
///     サブエミッターは自分の emitPosition からしか湧けなかった。«斬った位置で火花» や
///     «粒子が消えた場所から煙» は、発火した粒子の位置がここを通らないと原理的に作れない。
struct ParticleInjectedSpawn {
    /// 発火元のワールド位置。発生原点として据える (Shape のばらつきはこの点を中心に乗る)。
    math::Vector3 position;
    /// 発火元のワールド速度 [m/s]。継ぐ割合は受け側の subEmitterInheritVelocity が決める。
    math::Vector3 velocity;
    int           count = 1;
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

// 新しいエミッターが持つ内蔵の力。従来の既定 gravity = (0, -5, 0) と同じ落ち方をする。
// WHY 関数か: 集約初期化のメンバー既定値としてリストを 1 本用意するため。
//      «新しく置いた粒子が落ちない» は既定値の変更として最も気付かれにくい退行なので、
//      移行前と同じ加速度をここで固定する。
inline std::vector<ForceFieldSettings> MakeDefaultLocalForces()
{
    ForceFieldSettings gravity;
    gravity.fieldType = ForceFieldType::Wind;
    gravity.space     = ForceFieldSpace::World;
    gravity.direction = { 0.0f, -1.0f, 0.0f };
    gravity.strength  = 5.0f;
    gravity.radius    = 0.0f; // 無限 (減衰なし)
    return { gravity };
}

// Burst と内蔵の力は構造体の可変長リスト。IReflector の BeginObjectList /
// BeginObjectElement / EndObjectList がそのまま使える (SequencePlayerComponent と同じ形)。
// WHY 自由関数か: Reflect() の本体が長くなりすぎると «どこまでが 1 項目か» が読めなくなる。
inline void ReflectParticleBursts(IReflector& r, std::vector<ParticleBurst>& bursts)
{
    r.BeginField("bursts", "Bursts");
    const std::size_t count = r.BeginObjectList("Bursts", bursts.size());
    // 要素の中の Field は自分のキーで通す (ReflectForceFieldList と同じ理由)。
    r.EndField();
    bursts.resize(count);
    for (std::size_t index = 0; index < bursts.size(); ++index) {
        r.BeginObjectElement(index);
        r.Field("time", bursts[index].time);
        r.Field("count", bursts[index].count);
        r.Field("cycles", bursts[index].cycles);
        r.Field("interval", bursts[index].interval);
        r.Field("probability", bursts[index].probability);
        r.EndObjectElement();
    }
    const std::size_t removeIndex = r.EndObjectList();
    if (removeIndex < bursts.size())
        bursts.erase(bursts.begin() + static_cast<std::ptrdiff_t>(removeIndex));
    r.EndField();
}

// 内蔵の力の並びは ForceField::forces と同じ形。実体は ReflectForceFieldList 1 つで、
// ここはキー名 (localForces) を与えるだけ。
// WHY キーを分けるか: 同じ GameObject に «エミッター内蔵の力» と «シーンの力場» が
//      両方付くことがある。同じキー名だと TOML の同じ枠を奪い合う。
inline void ReflectLocalForces(IReflector& r, std::vector<ForceFieldSettings>& forces)
{
    ReflectForceFieldList(r, forces, "localForces", "Forces");
}

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
    // SubEmitter が積んだ «発火元つき» のスポーン。CPU スポーンがレート/バーストより先に
    // 消費し、消費したら空にする。積み手 (QueueSubEmitter) が maxParticles 相当で打ち切るので、
    // 受け側が一度も回らなくても (GPU / 非アクティブ) 無限には伸びない。
    std::vector<ParticleInjectedSpawn> injectedSpawns;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    std::string           loadedTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> motionVectorTexture;
    std::string           loadedMotionVectorTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> distortionTexture;
    std::string           loadedDistortionTexturePath;
    // 6 方向ライトマップの Negative 側 (.mat の emissive スロット)。Positive は albedo。
    renderer::ResourceHandle<renderer::TextureTag> sixWayNegativeTexture;
    std::string           loadedSixWayNegativeTexturePath;
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
    // WHY 所有しないか: 実体は ParticlePass が .mat 単位で 1 本だけ持ち、同じ .mat を使う
    //     エミッターが借りて回す。ここで返すと、他のエミッターの b2 まで道連れに落ちる。
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
    // 今フレームのスポーンデータ。ParticlePass のプールから借りているだけで、持ち主はプール
    // (シミュレーションのたびに借り直す。Release しないこと)。
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSpawnBuffer;
    // このエミッターに効く力場一式 (内蔵 + シーン)。定数バッファではなく SRV なので
    // 本数に上限が無い。gpuSpawnBuffer と同じくプールから借りたもの。
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuForceBuffer;
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


// ── ParticleEmitterSettings のモジュール ──
// WHY 塊に分けるか:
//   設定は 1 つの構造体に 100 以上の値が平たく並んでおり、«どの値が同じ仕事をするか» が
//   名前の接頭辞でしか読めなかった。仕事ごとに型へ畳めば、関数が受け取るのも
//   «尾の設定» や «カリングの設定» だけで済み、Inspector のモジュール欄とも一対一に揃う。
// NOTE: TOML のキーはフラットのまま (葉の名前 = キー)。既存の .scene / .particle / .vfx を
//       壊さないため、モジュール名は保存形式に出さない。葉の名前も変えない。

/// 距離と画面占有によるカリングと、発生量の LOD。
/// 粒子の現在 Bounds を使い、遠距離では発生数と描画数を段階的に削減する。
struct ParticleCullingSettings {
    bool  cullingEnabled          = true;
    float cullingBoundsPadding    = 0.25f;
    bool  lodEnabled              = true;
    float lodNearDistance         = 12.0f;
    float lodFarDistance          = 40.0f;
    float lodNearRateScale        = 1.0f;
    float lodFarRateScale         = 0.25f;
    float screenCoverageThreshold = 0.0f;
    bool  pauseWhenCulled         = false;
};

/// 粒子 1 つ 1 つに付ける尾 (per-particle Trail)。火の粉・魔法の軌跡のように «粒が線を引く» 表現用。
/// 既定は履歴点へビルボードを連ねる方式で、既存の描画 (シェーダー・PSO・テクスチャ・ブレンド) を
/// そのまま使えるので素材が揃う。太い帯が主役なら trailRibbon を使う。
/// @note GPU シミュレーションでは履歴を保持できないため、有効時は CPU へ縮退する。
struct ParticleTrailSettings {
    bool  trailEnabled        = false;
    int   trailPointCount     = 6;      // 使用する履歴点数 [1, kMaxParticleTrailPoints]
    float trailSampleInterval = 0.03f;  // 履歴を刻む間隔 [秒]。短いほど滑らか
    float trailWidthScale     = 0.6f;   // 尾の先端 (最古) 側のサイズ倍率
    float trailAlphaScale     = 0.5f;   // 尾の先端側の不透明度倍率
    math::Vector4 trailColorTint = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  trailRibbon         = false;
    // 帯の幅 [m]。0 以下なら粒子サイズをそのまま使う。
    // WHY: 帯は粒子サイズと独立に太さを決めたいことが多い (小さな火の粉が太い軌跡を引く等)。
    float trailRibbonWidth    = 0.0f;
};

/// 粒子を点光源にする (Niagara の Light Renderer 相当)。火の粉や魔法弾が地面・壁を照らす。
/// @note CPU シミュレーションの粒子だけが対象 (GPU の粒子は CPU から位置を読めない)。
///       光は RenderSystem のライト配列 (kMaxPunctualLights) を LightComponent と共有し、
///       LightComponent を先に積んだ残りの枠だけを使う。
struct ParticleLightSettings {
    bool  lightEnabled          = false;
    /// 光らせる粒子の割合 [0,1]。粒子ごとの固定乱数で決まるので寿命の間は変わらない。
    float lightRatio            = 1.0f;
    /// 1 エミッターから出す本数の上限。明るい順に選ぶ。
    int   lightMaxCount         = 8;
    float lightRange            = 2.0f;
    /// true なら lightRange × 粒子サイズを届く距離にする (大きな火の玉ほど遠くまで照らす)。
    bool  lightRangeFromSize    = false;
    float lightIntensity        = 1.0f;
    /// true なら lightColor × 粒子の色 (色のグラデーションに光の色も追従する)。
    bool  lightUseParticleColor = true;
    math::Vector4 lightColor    = { 1.0f, 1.0f, 1.0f, 1.0f };
    /// true なら粒子のアルファで強さを落とす (消えかけの粒子が光り続けない)。
    bool  lightFadeWithAlpha    = true;
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

    /// エミッターが内蔵する力。重力・空気抵抗・乱流・周回・放射がすべてここに入る。
    ///
    /// WHY 個別のフィールドをやめたか:
    ///   gravity / velocityDamping / noise* / orbital* / radialVelocity は
    ///   ForceField の Wind / Drag / Turbulence / Vortex / Repulse と
    ///   «同じ式» だった。型が違うだけで評価関数が CPU に 2 本・HLSL に 2 本あり、
    ///   力を 1 種類足すたびに 4 か所へ書く必要があった。同じ型にすれば、
    ///   シーンに置いた力場と内蔵の力を 1 本の評価器が区別せず処理できる。
    ///
    /// @note シーンに置いた力場と違い、これはエミッターに追従する固有の運動として効く。
    ///       channels は内蔵の力では意味を持たない (既に相手が 1 体に決まっている)。
    std::vector<ForceFieldSettings> localForces = MakeDefaultLocalForces();

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
    /// 空でなければビルボードの代わりに «メッシュ粒子» として描く (粒子 1 個 = 1 TRS)。
    ///
    /// @warning **このパスは一度もロードされない。** 実体の形は同じ GameObject の
    ///   MeshRenderer が持つ (CPU 経路は MeshTrailRenderPass、GPU 経路は ParticlePass の
    ///   インスタンス描画。どちらも MeshRenderer::mesh を読む)。ここへ別のモデルを
    ///   書いても黙って無視される ─ 意味を持つのは «空か / 空でないか» だけ。
    ///   本来は bool が正しい型だが、既存の .scene / .vfx / .particle にこのキーが
    ///   文字列で保存されているため、互換のために型は変えない。
    /// @note CPU 経路はさらに同じ GameObject の MeshTrailComponent を要求する
    ///   (描くのが MeshTrailRenderPass なので、それが無いと粒子が 1 つも出ない)。
    std::string meshParticlePath;
    // 粒子ごとの色ゆらぎ [0,1]。発生時に RGB を各チャンネル独立で ±colorVariation 倍する。
    // 完全に同色だと群れが一枚のベタ塗りに見える。チャンネル独立にすると明度差と
    // 軽い色相差が同時に出て厚みが出る。
    float colorVariation = 0.0f;
    float sizeCurvePower = 1.0f;
    float colorCurvePower = 1.0f;
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
    // 内蔵の Drag 力に掛ける時間倍率。噴き出し直後は素直に飛び、
    // 後半で急に空気抵抗が効く、といった減衰の作り分けに使う。
    // @note 掛かる相手は localForces の Drag 型すべて。シーンに置いた力場には効かない
    //       (あちらは «空間の性質» で、粒子の寿命とは無関係のため)。
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

    // 周回 (Vortex) と放射 (Repulse) は localForces に space = Emitter で入る。
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

    ParticleCullingSettings culling;

    // 乱流 (Turbulence) と速度場 (VectorField) も localForces に入る。
    // シーン内の ForceField から力を受けるか。
    // WHY: UI 演出用パーティクルなど、環境の風に反応させたくないエミッターを除外できるようにする。
    bool receiveForceFields = true;

    /// 受け取る力場を選ぶビットマスク。ForceField::channels と 1 ビットでも
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
    /// Physics 衝突で «当たってよい» コライダーのレイヤー集合 (ビット n = レイヤー n)。
    /// 既定は全ビット ON で従来どおり全レイヤーに当たる。
    /// @note トリガーはマスクに関わらず常に素通しする。トリガーは «通過を検知する体積» で
    ///       あって面ではないので、そこで跳ねると «見えない壁で火花が止まる» になる。
    uint32_t collisionLayerMask = 0xFFFFFFFFu;
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
    ParticleTrailSettings trail;
    ParticleLightSettings light;
    int subEmitterBurstCount = 1;
    /// 発火元の粒子の速度をこのエミッターの初速へ継ぐ割合 [0,1]。既定 0 = 継がない。
    ///
    /// WHY 既定を 0 にするか: 全部そのまま継ぐと、火花が発火元と同じ向きへ流れるだけの
    ///     «二番煎じ» になって «弾けた» 感じが出ない。当たりの火花は 0.1〜0.3 程度、
    ///     «崩れて散る» 煙のように流れを見せたいときだけ大きくする。
    /// @note 効くのは注入されたスポーン (birth / death / collision) だけ。エミッター自身の
    ///       移動を継ぐのは inheritVelocity で、別の値。
    float subEmitterInheritVelocity = 0.0f;
    // 名前引きの探索範囲を限定するルート GameObject。INVALID でシーン全体。
    // 同じ .vfx を複数配置すると同名の GO が並ぶので、シーン全体で引くと隣のインスタンスを
    // 吹かせてしまう。VFXSystem が頭出し (ResetSubtree) のたびに配下へ VFX ルートを配る。
    // シーンへ手で置いた Emitter は INVALID のままで、従来どおりシーン全体から引く。
    // ランタイム専用。シーン保存対象ではない。
    EntityID subEmitterScopeRoot = EntityID::INVALID;
    std::string birthSubEmitter;
    std::string deathSubEmitter;
    std::string collisionSubEmitter;

    // ── 内蔵の力を «役割» で引く ──
    // WHY 添字で扱わないか: Inspector の Gravity 欄・プリセット・ScriptProxy の
    //     SetGravity はいずれも «下向きの一定加速 1 本» を編集する UI で、それが
    //     リストの何番目かを知らない。役割で引ければ、保存形式がリストに変わっても
    //     呼び出し側は «重力» のまま書ける。同じ型が複数あれば先頭を返す。

    [[nodiscard]] const ForceFieldSettings* FindLocalForce(ForceFieldType type) const
    {
        for (const auto& force : localForces)
            if (force.fieldType == type) return &force;
        return nullptr;
    }

    [[nodiscard]] ForceFieldSettings* FindLocalForce(ForceFieldType type)
    {
        for (auto& force : localForces)
            if (force.fieldType == type) return &force;
        return nullptr;
    }

    /// 無ければ既定値で 1 本足して返す。space は型ごとに «その力が意味を持つ座標系» へ倒す。
    ForceFieldSettings& EnsureLocalForce(ForceFieldType type)
    {
        if (ForceFieldSettings* existing = FindLocalForce(type)) return *existing;
        ForceFieldSettings force;
        force.fieldType = type;
        force.radius    = 0.0f; // 内蔵の力は既定で «エミッター全体に効く»
        // 周回と放射はエミッター原点が要る。それ以外は世界の性質なので World のまま。
        force.space = (type == ForceFieldType::Vortex
                    || type == ForceFieldType::Repulse
                    || type == ForceFieldType::Attract)
                        ? ForceFieldSpace::Emitter
                        : ForceFieldSpace::World;
        if (type == ForceFieldType::Drag || type == ForceFieldType::Turbulence)
            force.strength = 0.0f; // 足しただけで挙動が変わらないように
        localForces.push_back(force);
        return localForces.back();
    }

    void RemoveLocalForce(ForceFieldType type)
    {
        localForces.erase(
            std::remove_if(localForces.begin(), localForces.end(),
                           [type](const ForceFieldSettings& f) { return f.fieldType == type; }),
            localForces.end());
    }

    /// 重力加速度 [m/s^2]。内蔵の Wind 力を «向き × 強さ» として読む。
    [[nodiscard]] math::Vector3 GravityAcceleration() const
    {
        const ForceFieldSettings* wind = FindLocalForce(ForceFieldType::Wind);
        return wind ? wind->direction * wind->strength : math::Vector3::ZERO;
    }

    /// 重力加速度を書く。長さ 0 なら向きを保ったまま強さだけ 0 にする。
    /// WHY 向きを残すか: Inspector で一度 0 にすると向きが失われ、値を戻したときに
    ///     +X へ飛ぶ。「0 にした」は「向きを忘れてよい」ではない。
    void SetGravityAcceleration(const math::Vector3& acceleration)
    {
        ForceFieldSettings& wind = EnsureLocalForce(ForceFieldType::Wind);
        const float magnitude = acceleration.Length();
        if (magnitude > 1.0e-6f) {
            wind.direction = acceleration * (1.0f / magnitude);
            wind.strength  = magnitude;
        } else {
            wind.strength = 0.0f;
        }
        wind.radius = 0.0f;
        wind.space  = ForceFieldSpace::World;
    }

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
        r.Field("angularVelocityMin", angularVelocityMin);
        r.Field("angularVelocityMax", angularVelocityMax);
        r.Field("renderPriority", renderPriority);
        r.Field("sizeAxisScale", sizeAxisScale);
        r.Field("colorVariation", colorVariation);
        r.Field("inheritVelocity", inheritVelocity);
        r.Field("useSizeCurve", useSizeCurve);
        r.Field("useVelocityCurve", useVelocityCurve);
        r.Field("useColorGradient", useColorGradient);
        r.Field("rateOverDistance", rateOverDistance);
        r.Field("prewarm", prewarm);
        int gradientColorSpaceValue = static_cast<int>(colorGradient.colorSpace);
        r.Field("gradientColorSpace", gradientColorSpaceValue);
        colorGradient.colorSpace = static_cast<ParticleColorSpace>(
            gradientColorSpaceValue < 0 ? 0 : (gradientColorSpaceValue > 2 ? 2 : gradientColorSpaceValue));
        r.Field("cullingEnabled", culling.cullingEnabled);
        r.Field("cullingBoundsPadding", culling.cullingBoundsPadding);
        r.Field("lodEnabled", culling.lodEnabled);
        r.Field("lodNearDistance", culling.lodNearDistance);
        r.Field("lodFarDistance", culling.lodFarDistance);
        r.Field("lodNearRateScale", culling.lodNearRateScale);
        r.Field("lodFarRateScale", culling.lodFarRateScale);
        r.Field("screenCoverageThreshold", culling.screenCoverageThreshold);
        r.Field("pauseWhenCulled", culling.pauseWhenCulled);
        r.Field("receiveForceFields", receiveForceFields);
        // IReflector は符号なし整数を扱わない。ビットパターンは int 経由でも保たれる。
        int channelsValue = static_cast<int>(forceFieldChannels);
        r.Field("forceFieldChannels", channelsValue);
        forceFieldChannels = static_cast<uint32_t>(channelsValue);

        // ── ここから下は以前 Reflect に無く、コーデックにしか居なかった 44 項目 ──
        // WHY 揃える必要があるか: シーン保存はコーデック経由だが、AI バス
        //     (EditorBusDispatcher) と汎用 Inspector は Reflect() を使う。片方にしか
        //     無いフィールドは「保存はされるのに外から読み書きできない」状態になり、
        //     しかもエラーが出ないので存在自体に気付けない。正本を 1 つにする。

        // 速度による見た目
        r.Field("speedRange", speedRange);
        r.Field("useSpeedSizeCurve", useSpeedSizeCurve);
        r.Field("useSpeedColorGradient", useSpeedColorGradient);
        r.Field("useRotationCurve", useRotationCurve);
        r.Field("useDragCurve", useDragCurve);
        r.Field("useEmitRateCurve", useEmitRateCurve);

        // 衝突
        int collisionModeValue = static_cast<int>(collisionMode);
        r.Field("collisionMode", collisionModeValue);
        collisionMode = static_cast<ParticleCollisionMode>(std::clamp(collisionModeValue, 0, 3));
        int collisionResponseValue = static_cast<int>(collisionResponse);
        r.Field("collisionResponse", collisionResponseValue);
        collisionResponse = static_cast<ParticleCollisionResponse>(std::clamp(collisionResponseValue, 0, 2));
        r.Field("collisionRadius", collisionRadius);
        r.Field("collisionBounciness", collisionBounciness);
        r.Field("collisionDamping", collisionDamping);
        r.Field("collisionPlaneY", collisionPlaneY);
        // forceFieldChannels と同じ理由で int 経由 (IReflector は符号なし整数を扱わない)。
        int collisionLayersValue = static_cast<int>(collisionLayerMask);
        r.Field("collisionLayerMask", collisionLayersValue);
        collisionLayerMask = static_cast<uint32_t>(collisionLayersValue);

        // 黒体放射
        r.Field("blackbodyEnabled", blackbodyEnabled);
        r.Field("blackbodyReferenceTemperature", blackbodyReferenceTemperature);
        r.Field("blackbodyIntensity", blackbodyIntensity);

        // per-particle トレイル
        r.Field("trailEnabled", trail.trailEnabled);
        r.Field("trailPointCount", trail.trailPointCount);
        r.Field("trailSampleInterval", trail.trailSampleInterval);
        r.Field("trailWidthScale", trail.trailWidthScale);
        r.Field("trailAlphaScale", trail.trailAlphaScale);
        r.ColorField("trailColorTint", trail.trailColorTint);
        r.Field("trailRibbon", trail.trailRibbon);
        r.Field("trailRibbonWidth", trail.trailRibbonWidth);

        // 粒子を点光源にする
        r.Field("lightEnabled", light.lightEnabled);
        r.Field("lightRatio", light.lightRatio);
        r.Field("lightMaxCount", light.lightMaxCount);
        r.Field("lightRange", light.lightRange);
        r.Field("lightRangeFromSize", light.lightRangeFromSize);
        r.Field("lightIntensity", light.lightIntensity);
        r.Field("lightUseParticleColor", light.lightUseParticleColor);
        r.ColorField("lightColor", light.lightColor);
        r.Field("lightFadeWithAlpha", light.lightFadeWithAlpha);

        // サブエミッター (scopeRoot はランタイム専用なので出さない)
        r.Field("birthSubEmitter", birthSubEmitter);
        r.Field("deathSubEmitter", deathSubEmitter);
        r.Field("collisionSubEmitter", collisionSubEmitter);
        r.Field("subEmitterBurstCount", subEmitterBurstCount);
        r.Field("subEmitterInheritVelocity", subEmitterInheritVelocity);

        // カーブとグラデーション。IReflector は専用の仮想関数を持っているので、
        // コーデックの手書きと同じ形をそのまま表現できる。
        r.BeginField("sizeCurve", "Size over Lifetime");
        r.Field("sizeCurve", sizeCurve);
        r.EndField();
        r.Field("velocityCurve", velocityCurve);
        r.Field("rotationCurve", rotationCurve);
        r.Field("dragCurve", dragCurve);
        r.Field("emitRateCurve", emitRateCurve);
        r.Field("speedSizeCurve", speedSizeCurve);
        r.BeginField("temperatureCurve", "Temperature (K)");
        r.SetFieldMax(3000.0f);
        r.Field("temperatureCurve", temperatureCurve);
        r.EndField();
        r.Field("colorGradient", colorGradient);
        r.Field("speedColorGradient", speedColorGradient);

        ReflectParticleBursts(r, bursts);
        ReflectLocalForces(r, localForces);
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
        runtime.injectedSpawns.clear();
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
        runtime.injectedSpawns.clear();
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
        runtime.injectedSpawns.clear();
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
