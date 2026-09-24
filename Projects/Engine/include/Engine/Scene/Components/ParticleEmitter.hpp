/// @file    ParticleEmitter.hpp
/// @brief   パーティクルの発生・寿命・見た目の設定一式。描画リソースは Renderer 側が持つ。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#pragma once
#include <Graphics/Effects/ParticleDrawTypes.hpp>
#include <Engine/Asset/ParticleMaterialSettings.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Entity.hpp>
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

/// @note ParticleCurve / ParticleGradient は Script からも宣言できるよう別ヘッダーに住む。

using renderer::GpuParticle;
using renderer::GpuSpawnEntry;
using renderer::kMaxParticleTrailPoints;
using renderer::Particle;
/// @brief 発火元から注入されたスポーン 1 件。SubEmitter が «どこで・どう動いていたか» を渡す口。
/// @note burstPending は «何個出すか» しか運べず、サブエミッターは自分の emitPosition からしか
/// @note 湧けなかった。«斬った位置で火花» のような発火位置依存の演出はこれが無いと作れない。
struct ParticleInjectedSpawn {
    /// @brief 発火元のワールド位置。発生原点として据える (Shape のばらつきはこの点を中心に乗る)。
    math::Vector3 position;
    /// @brief 発火元のワールド速度 [m/s]。継ぐ割合は受け側の subEmitterInheritVelocity が決める。
    math::Vector3 velocity;
    int           count = 1;
};

/// @brief ParticleBurst — 再生時間上の繰り返しBurst設定。
struct ParticleBurst {
    float time = 0.0f;
    int count = 10;
    int cycles = 1;
    float interval = 0.1f;
    float probability = 1.0f;
};

/// @brief MeshShapeVertex — MeshSurface が静的頂点とスキンウェイトを共通形式で保持する。
/// @note AssetManager 所有の Model を参照し続けず、スポーン時は必要な頂点情報だけを高速に抽選する。
struct MeshShapeVertex {
    math::Vector3 position;
    math::Vector3 normal;
    uint32_t boneIndices[4] = {};
    float boneWeights[4] = {};
    bool skinned = false;
};

/// @brief MeshShapeTriangle — MeshSurface の面積重み抽選 1 枚ぶん。頂点は meshShapeVertices への添字。
/// @brief 頂点を一様抽選すると «頂点密度» に比例して湧き、細かい部位からしか粒が出ない。
/// @brief 面積に比例させ、三角形内部を重心座標で取れば低ポリでも表面が埋まる。
/// @brief 重みはバインドポーズ面積で足りる (スキニングで伸縮しても抽選の比率はほぼ変わらない)。
struct MeshShapeTriangle {
    uint32_t indices[3] = {};
    /// @brief 先頭からこの三角形までの面積の総和。1 回の乱数を二分探索で引くための累積分布。
    float    cumulativeArea = 0.0f;
};

/// @brief 新しいエミッターの既定の重力加速度 [m/s^2]。
/// @note 旧 MakeDefaultLocalForces が «下向き strength 5 の Wind» として場に相乗りさせていた値。
/// @note 重力は媒質の運動ではないので、場ではなくエミッターの加速度として持つ
/// @note (物理の 9.81 と違うことに気付けなかったのは、場に紛れていたため)。
inline constexpr math::Vector3 kDefaultParticleGravity = { 0.0f, -5.0f, 0.0f };

/// @brief Burst と内蔵の力は構造体の可変長リスト。IReflector の BeginObjectList /
/// @brief BeginObjectElement / EndObjectList がそのまま使える (SequencePlayerComponent と同じ形)。
/// @note 自由関数にしたのは、Reflect() の本体が長くなりすぎると «どこまでが 1 項目か» が
/// @note 読めなくなるため。
inline void ReflectParticleBursts(IReflector& r, std::vector<ParticleBurst>& bursts)
{
    r.BeginField("bursts", "Bursts");
    const std::size_t count = r.BeginObjectList("Bursts", bursts.size());
    /// @note 要素の中の Field は自分のキーで通す (ReflectFlowFieldList と同じ理由)。
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

/// @brief 内蔵の流れの並びは FlowField::forces と同じ形。実体は ReflectFlowFieldList 1 つで、
/// @brief ここはキー名 (localForces) を与えるだけ。
/// @note 同じ GameObject に «エミッター内蔵の流れ» と «シーンの場» が両方付くことがあり、
/// @note 同じキー名だと TOML の同じ枠を奪い合うため分ける。
inline void ReflectLocalForces(IReflector& r, std::vector<FlowFieldSettings>& forces)
{
    ReflectFlowFieldList(r, forces, "localForces", "Forces");
}

/// @brief materialPath 未設定の Emitter が使う既定 .mat。加算の丸い光。
/// @brief 何も無いと 1x1 白 (alpha=1) へ落ち、粒子が「不透明な四角」として描かれる。
/// @brief これが無いプロジェクトでは従来どおり白へ落ちる (存在しなくても壊れない)。
inline constexpr const char* PARTICLE_FALLBACK_MATERIAL =
    "Assets/Materials/Particles/ParticleFallback.mat";

/// @brief ParticleEmitter のランタイム状態。シーンに保存しない一切をここへ集める。
/// @note オーサリング設定 (ParticleEmitterSettings) と分けるのは、GameObject 複製時に
/// @note GPU バッファ・粒子列などのハンドルを新旧で共有させないため。
/// @note コピー代入は常に初期状態を作る (複製のたびにリセットを書かなくて済む規則)。
struct ParticleRuntime {
    std::vector<Particle> particles;
    /// @brief emitRate * dt の累積値。1.0 を超えるたびに 1 粒子を発生させる。
    /// @brief こうすることで低フレームレートでも発生数が dt に比例して安定する。
    float                 emitAccum = 0.0f;
    /// @brief randomSeed から初期化されるランタイム状態。シーン保存対象ではない。
    uint32_t              randomState = 1;
    float                 playTime = 0.0f;
    float                 delayTime = 0.0f;
    int                   burstPending = 0;
    /// @brief SubEmitter が積んだ «発火元つき» のスポーン。CPU スポーンがレート/バーストより先に
    /// @brief 消費し、消費したら空にする。積み手 (QueueSubEmitter) が maxParticles 相当で打ち切るので、
    /// @brief 受け側が一度も回らなくても (GPU / 非アクティブ) 無限には伸びない。
    std::vector<ParticleInjectedSpawn> injectedSpawns;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    std::string           loadedTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> motionVectorTexture;
    std::string           loadedMotionVectorTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> distortionTexture;
    std::string           loadedDistortionTexturePath;
    /// @brief 6 方向ライトマップの Negative 側 (.mat の emissive スロット)。Positive は albedo。
    renderer::ResourceHandle<renderer::TextureTag> sixWayNegativeTexture;
    std::string           loadedSixWayNegativeTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> sixWayAlbedoColorTexture;
    std::string           loadedSixWayAlbedoColorTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> sixWayEmissionColorTexture;
    std::string           loadedSixWayEmissionColorTexturePath;
    std::string           loadedMaterialPath; ///< @brief materialPath の変更検出用。シーン保存対象外。

    /// @brief .mat が shader を指定していたときの描画シェーダー。無効なら組み込み Particle.hlsl。
    /// @brief 組み込みは「テクスチャを貼ったビルボード」しか出せないので、放電のように手続きで
    /// @brief 形を作る素材は差し替える。
    /// @brief 差し替えたシェーダーは Material/Effects/ParticleMaterial.hlsli を include し、
    /// @brief .mat には render_path = "particle" を書くこと。フリップブック・歪み・煙は自前で書く。
    renderer::ResourceHandle<renderer::ShaderTag> customShader;
    std::string           loadedShaderPath;   ///< @brief customShader の変更検出用。シーン保存対象外。
    /// @brief カスタムシェーダーが宣言した MaterialConstants (b2) へ流す .mat の [params]。
    /// @brief 組み込みシェーダーは MaterialConstants を宣言しないので無効ハンドルのまま。
    /// @note 所有しない: 実体は ParticlePass が .mat 単位で 1 本だけ持ち、エミッターは借りて回す。
    /// @note ここで返すと他のエミッターの b2 まで道連れに落ちる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialParamsCB;

    /// @brief .mat から解決した «見た目» 一式。シミュレーションも描画もこちらだけを見る。
    /// @brief settings はシーン保存対象なので書き戻さない («触っていないのに保存内容が変わる» /
    /// @brief «Inspector で変えても次のフレームで戻る» が起きる)。
    asset::ParticleMaterialSettings material;
    /// @brief .mat の blend_mode から解決した実効ブレンド。PSO 選択はこれを見る。
    ParticleBlendMode resolvedBlend = ParticleBlendMode::Additive;
    /// @brief albedo テクスチャが sRGB でエンコードされているか (.meta の srgb)。
    /// @note 手描き素材は sRGB、ProceduralVFXTextures が焼くものはリニア。一律にリニア化すると
    /// @note 後者が暗く沈むため、素材ごとの実際の値に従う。
    bool                  textureIsSrgb = true;
    /// @brief .mat の [params] albedo。リニアへ変換済み。
    math::Vector4         materialTint = { 1.0f, 1.0f, 1.0f, 1.0f };
    /// @brief 黒体放射を焼き込んだ後の実効グラデーション。blackbodyEnabled が false なら
    /// @brief colorGradient のコピー。シミュレーションと GPU 定数バッファはこちらだけを見る。
    ParticleGradient      runtimeGradient;
    /// @brief RefreshRuntimeGradient の再計算判定に使う入力の写し。シーン保存対象外。
    bool                                                       runtimeGradientValid = false;
    bool                                                       runtimeGradientBlackbody = false;
    float                                                      runtimeGradientReference = 0.0f;
    float                                                      runtimeGradientIntensity = 0.0f;
    std::array<ParticleGradientKey, kMaxParticleCurveKeys>     runtimeGradientSourceKeys{};
    uint32_t                                                   runtimeGradientSourceCount = 0;
    std::array<ParticleCurveKey, kMaxParticleCurveKeys>        runtimeGradientTemperatureKeys{};
    uint32_t                                                   runtimeGradientTemperatureCount = 0;

    /// @brief GPU パーティクル実行時状態 (シーン保存不要、デバイスリセット時に再生成)
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuParticleBuffer; ///< @brief RWStructuredBuffer: CS が更新
    /// @brief 今フレームのスポーンデータ。ParticlePass のプールから借りているだけで、持ち主はプール
    /// @brief (シミュレーションのたびに借り直す。Release しないこと)。
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSpawnBuffer;
    /// @brief このエミッターに効く力場一式 (内蔵 + シーン)。定数バッファではなく SRV なので
    /// @brief 本数に上限が無い。gpuSpawnBuffer と同じくプールから借りたもの。
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuForceBuffer;
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuEmitterCB;      ///< @brief CS 用エミッター定数バッファ
    renderer::ResourceHandle<renderer::ConstantBufferTag>   renderCB;          ///< @brief VS/PS 描画モード・Soft Particle
    /// @brief GPU ソート。sortMode != None のときだけ確保する。並べ替えるのは (キー, 粒子 index)
    /// @brief の対だけ (プール自体を動かすとリングバッファ位置が変わってスポーンが壊れる)。
    /// @brief 連続リボン (trailRibbon)。帯の頂点は毎フレーム CPU で作り直す (GPU では履歴を持てない)。
    /// @brief 頂点バッファは描画側の DynamicVertexBufferPool から借りる。帯はビューごとに形が
    /// @brief 変わるので、エミッターに 1 本持たせると後のビューが先のビューの Draw を書き替える。
    renderer::ResourceHandle<renderer::ConstantBufferTag>   trailRibbonCB;
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSortBuffer;     ///< @brief `RWStructuredBuffer<uint2>`
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuSortCB;         ///< @brief bitonic の (k, j) を段ごとに更新
    uint32_t gpuSortCapacity = 0;   ///< @brief gpuSortBuffer の要素数 (2 のべき乗、maxParticles 以上)
    uint32_t gpuWriteHead    = 0;   ///< @brief gpuSpawnBuffer の次書き込み位置 (リングバッファインデックス)
    uint32_t gpuSpawnCount   = 0;   ///< @brief 今フレームのスポーン数
    bool     gpuInitialized  = false;
    uint32_t gpuCapacity = 0;
    bool     gpuClearPending = false;
    uint64_t gpuResetVersion = 0;   ///< @brief 最後に確認した ResourceManager::GetResetVersion()
    uint64_t lastGpuSimulationFrame = UINT64_MAX;
    uint64_t lastCpuSimulationFrame = UINT64_MAX;
    uint64_t lastPlaybackFrame = UINT64_MAX;
    bool emitThisFrame = false;
    bool prewarmed = false;
    math::Vector3 boundsCenter = {};
    float boundsRadius = 0.0f;
    /// @note GPU 粒子の位置を包含する球と速さの上限。描画用の推定 Bounds では力場を落とさない。
    math::Vector3 flowBoundsCenter = {};
    float flowBoundsRadius = 0.0f;
    float flowSpeedBound = 0.0f;
    float flowMinimumAge = 0.0f;
    float flowMaxLifetime = 0.0f;
    float lodRateScale = 1.0f;
    int visibleParticleCount = 0;
    bool isCulledThisFrame = false;
    int prewarmSpawnPending = 0;
    bool hasLastEmitterPosition = false;
    math::Vector3 lastEmitterPosition = {};
    /// @brief エミッター自身のワールド速度 [m/s]。inheritVelocity がスポーン時に参照する。
    /// @brief ParticleSimulationSystem が lastEmitterPosition を更新するのと同じ場所で毎フレーム求める。
    math::Vector3 emitterVelocity = {};
    float distanceEmitAccum = 0.0f;
    std::vector<int> burstCyclesFired;

    /// @brief MeshSurface Shapeのランタイムキャッシュ。位置・法線と4ボーンウェイトだけを複製する。
    std::vector<MeshShapeVertex>   meshShapeVertices;
    /// @brief 面積の累積分布付き三角形列。インデックスを持たないメッシュでは空のままで、
    /// @brief その場合は頂点の一様抽選へ縮退する。
    std::vector<MeshShapeTriangle> meshShapeTriangles;
    std::string loadedMeshShapePath;
    int         loadedMeshShapeIndex = -2;

    /// @name エディタープレビュー制御 (VFX Editor 用ランタイム状態。シーン保存対象外)
    /// @{
    /// @note VFX Editor の再生速度・一時停止をシミュレーション dt へ注入する口。パネルが毎フレーム
    /// @note 書き込み、途絶えたら自動で通常速度へ戻る (パネルを閉じても凍結が残らないフェイルセーフ)。
    float    editorTimeScale      = 1.0f;
    uint64_t editorTimeScaleFrame = 0;     ///< @brief editorTimeScale を最後に書き込んだ Time::frameCount
    /// @brief タイムラインスクラブ要求 [秒]。>=0 のとき ParticleSimulationSystem が消費し、
    /// @brief randomSeed から決定論的にその時刻まで再シミュレートする。負値 = 要求なし。
    float    editorScrubTime      = -1.0f;

    /// @brief 当たり・死亡の「今フレームぶん」の計数。VFX Graph の OnCollision / OnDeath が消費する。
    int collisionCountThisFrame = 0;
    int deathCountThisFrame = 0; ///< @brief VFX GraphのOnDeath eventがフレーム単位で消費する。
    ParticleRuntime() = default;
    ParticleRuntime(ParticleRuntime&&) noexcept = default;
    ParticleRuntime& operator=(ParticleRuntime&&) noexcept = default;

    /// @brief コピーは「初期状態」を作る。GPU ハンドルや粒子列を引き継がせないための規則。
    ParticleRuntime(const ParticleRuntime&) {}
    ParticleRuntime& operator=(const ParticleRuntime&)
    {
        /// @note 一時からの move 代入。常に初期状態へ戻す
        *this = ParticleRuntime{};
        return *this;
    }
    /// @}
};


/// @note 以下、設定を仕事ごとの型へ塊分けする。100 以上の値が接頭辞でしか区別できなかったため。
/// @note TOML のキーはフラットのまま (葉の名前 = キー) で、モジュール名は保存形式に出ない。

/// @brief 距離と画面占有によるカリングと、発生量の LOD。
/// @brief 粒子の現在 Bounds を使い、遠距離では発生数と描画数を段階的に削減する。
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

/// @brief 粒子 1 つ 1 つに付ける尾 (per-particle Trail)。火の粉・魔法の軌跡のように «粒が線を引く» 表現用。
/// @brief 既定は履歴点へビルボードを連ねる方式で、既存の描画 (シェーダー・PSO・テクスチャ・ブレンド) を
/// @brief そのまま使えるので素材が揃う。太い帯が主役なら trailRibbon を使う。
/// @note GPU シミュレーションでは履歴を保持できないため、有効時は CPU へ縮退する。
using renderer::ParticleTrailSettings;

/// @brief 粒子を点光源にする (Niagara の Light Renderer 相当)。火の粉や魔法弾が地面・壁を照らす。
/// @note CPU シミュレーションの粒子だけが対象 (GPU の粒子は CPU から位置を読めない)。
/// @note 光は RenderSystem のライト配列 (kMaxPunctualLights) を LightComponent と共有し、
/// @note LightComponent を先に積んだ残りの枠だけを使う。
struct ParticleLightSettings {
    bool  lightEnabled          = false;
    /// @brief 光らせる粒子の割合 [0,1]。粒子ごとの固定乱数で決まるので寿命の間は変わらない。
    float lightRatio            = 1.0f;
    /// @brief 1 エミッターから出す本数の上限。明るい順に選ぶ。
    int   lightMaxCount         = 8;
    float lightRange            = 2.0f;
    /// @brief true なら lightRange × 粒子サイズを届く距離にする (大きな火の玉ほど遠くまで照らす)。
    bool  lightRangeFromSize    = false;
    float lightIntensity        = 1.0f;
    /// @brief true なら lightColor × 粒子の色 (色のグラデーションに光の色も追従する)。
    bool  lightUseParticleColor = true;
    math::Vector4 lightColor    = { 1.0f, 1.0f, 1.0f, 1.0f };
    /// @brief true なら粒子のアルファで強さを落とす (消えかけの粒子が光り続けない)。
    bool  lightFadeWithAlpha    = true;
};

/// @brief エミッターのオーサリング設定。GameObject に依存しない値の塊。
/// @note VFXGraphNode 等 GameObject を持たない側とも共有するため、コンポーネントと型を分けた
/// @note (`emitter.settings = node.particle;` で写せる)。直列化の正本もこの構造体 1 つに揃える。
struct ParticleEmitterSettings {

    math::Vector3 emitPosition   = {};
    math::Vector3 emitVelocity   = { 0.0f, 4.0f, 0.0f };
    /// @brief 初速のばらつき [m/s]。emitVelocity の 3 軸へ等方に ±velocitySpread を加える。
    /// @brief Sphere / Cone 形状が持つ方向成分 (shapeVelocity) とは別枠で、こちらは純粋な揺らぎ。
    float         velocitySpread = 1.5f;
    math::Vector4 colorStart     = { 1.0f, 0.7f, 0.2f, 1.0f };
    math::Vector4 colorEnd       = { 1.0f, 0.1f, 0.0f, 0.0f };
    float         sizeStart      = 0.4f;
    float         sizeEnd        = 0.05f;
    /// @brief ビルボードの縦横比。size に対する軸ごとの倍率で、xy のみ使う (z は Mesh Particle 用)。
    /// @brief エミッター単位の値なので per-particle ではなく定数バッファへ載せる。
    math::Vector3 sizeAxisScale  = { 1.0f, 1.0f, 1.0f };
    float         lifetime       = 2.0f;
    float         lifetimeRandom = 0.0f;
    float         emitRate       = 30.0f;
    int           maxParticles   = 300;

    /// @brief エミッターが内蔵する流れ。乱流・周回・放射・焼いた場がここに入る。
    /// @note FlowField と同じ式を持つため個別フィールドをやめて統合した。エミッターに追従する
    /// @note 固有の流れとして効き、channels は無関係 (相手は既に 1 体に決まっている)。重力・
    /// @note 空気抵抗は媒質の運動ではないため gravity / flowCoupling が別に持つ。
    std::vector<FlowFieldSettings> localForces;

    /// @brief 粒子だけに掛かる加速度 [m/s^2]。重力・浮力の «上昇» をここで表す。
    /// @note 正本は物理の ProjectSettings ではない。粒子は «絵» なので落ち方を個別に決める。
    math::Vector3 gravity = kDefaultParticleGravity;

    /// @brief 流れへの結合係数 [1/s]。v += (v_flow − v) · flowCoupling · dt。
    /// @note 旧 «内蔵 Drag の strength» と同じ枠・同じ単位。空気抵抗は流れの種類ではなく
    /// @note «この粒子がどれだけ流れに乗るか» なので、型ではなくエミッターの設定にした。
    /// @note Drag over Lifetime カーブはこの値に掛かる。
    float flowCoupling = kDefaultFlowCoupling;

    /// @note std::rand() のグローバル状態を避け、エミッター単位で再現可能な分布にする。
    uint32_t      randomSeed     = 1;
    bool          enabled        = true;

    /// @brief 再生状態。enabled は Component の有効/無効、playing はエフェクト再生を表す。
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
    /// @brief FBX / Model の頂点群を発生位置として使う。meshIndex < 0 なら全サブメッシュを結合する。
    /// @note CPU/GPU の両モードで同じスポーンバッファを使い、モデル形状 Particle を同じ見た目にする。
    std::string meshShapePath;
    int         meshShapeIndex = -1;
    float       meshShapeScale = 1.0f;
    /// @brief 同じ GameObject の AnimatorComponent が持つ現在のボーン行列で発生点を変形する。
    bool        meshShapeFollowSkinnedAnimation = false;
    /// @brief 表面法線方向へ与える初速 [m/s]。0 なら emitVelocity だけで飛ぶ。
    /// @brief 表面から «にじみ出る» 崩壊は、法線が初速に入って初めて成立する。
    /// @brief 負値は «表面へ吸い込む» 収束エフェクトになる。
    float       meshShapeNormalVelocity = 0.0f;

    ParticleSortMode  sortMode  = ParticleSortMode::None;
    /// @brief エミッター間の描画順。小さいほど先に描かれる (＝奥に見える)。
    /// @brief sortMode は 1 エミッター内しか並べ替えないので、別エミッターが重なる構成では
    /// @brief GameObject の並び順で前後が決まってしまう。同値ならカメラから遠い順。
    int renderPriority = 0;
    ParticleSimulationMode simulationMode = ParticleSimulationMode::Cpu;
    ParticleSimulationSpace simulationSpace = ParticleSimulationSpace::World;
    ParticleRenderMode renderMode = ParticleRenderMode::Billboard;
    float stretchedVelocityScale = 0.1f;
    float stretchedLengthScale = 1.0f;

    /// @brief CollisionはCPUで自作Physics WorldをQueryする。GPU指定時は正確性優先でCPUへ縮退する。

    /// @brief 見た目の正本となる .mat アセットへの参照。ブレンド・アルベド・フリップブック・
    /// @brief 歪み・煙・自己影・明るさはすべてここから来る (解決結果は runtime.material)。
    /// @brief 同じ素材を複数のエミッターで共有するため .mat 側に持つ。
    /// @brief 空のときは PARTICLE_FALLBACK_MATERIAL が使われる (白い矩形にはならない)。
    std::string materialPath;
    /// @brief 空でなければビルボードの代わりに «メッシュ粒子» として描く (粒子 1 個 = 1 TRS)。
    /// @warning このパスは読み込まれない。実体は同じ GameObject の MeshRenderer::mesh (bool が
    /// @note 正しい型だが .scene 等で文字列保存済みのため互換で型を変えない)。意味を持つのは空か否かだけ。
    /// @note CPU 経路はさらに同じ GameObject の MeshTrailComponent を要求する。
    std::string meshParticlePath;
    /// @brief 粒子ごとの色ゆらぎ [0,1]。発生時に RGB を各チャンネル独立で ±colorVariation 倍する。
    /// @brief 完全に同色だと群れが一枚のベタ塗りに見える。チャンネル独立にすると明度差と
    /// @brief 軽い色相差が同時に出て厚みが出る。
    float colorVariation = 0.0f;
    float sizeCurvePower = 1.0f;
    float colorCurvePower = 1.0f;
    float angularVelocityMin = 0.0f;
    float angularVelocityMax = 0.0f;
    /// @brief false なら生成時の板の向きを 0 rad に固定する。角速度には影響しない。
    bool randomStartRotation = true;
    bool useSizeCurve = false;
    ParticleCurve sizeCurve;
    bool useVelocityCurve = false;
    ParticleCurve velocityCurve;
    bool useColorGradient = false;
    ParticleGradient colorGradient;

    /// @brief 角速度に掛ける時間倍率。定数の angularVelocity だけでは
    /// @brief 「勢いよく回り始めて減速する」火の粉・破片の動きが作れない。
    bool useRotationCurve = false;
    ParticleCurve rotationCurve;
    /// @brief flowCoupling に掛ける時間倍率。噴き出し直後は素直に飛び、
    /// @brief 後半で急に流れへ乗る、といった作り分けに使う。
    bool useDragCurve = false;
    ParticleCurve dragCurve;

    /// @name 発生量の時間変化
    /// @{
    /// @brief emitRate に掛ける倍率。エミッターの再生時刻を duration で正規化した 0..1 で引く。
    /// @brief burst を刻んで近似すると粒の湧き方が段になって «脈打つ» ように見える。
    /// @brief loop するエミッターでは playTime を duration で割った余りで引く。
    bool useEmitRateCurve = false;
    ParticleCurve emitRateCurve;
    /// @}

    /// @name 速度による見た目
    /// @{
    /// @brief 粒の «速さ» で大きさと色を変える。寿命ベースのカーブとは別の軸。
    /// @brief 火花は «速いものほど明るく長い» のであって «出てすぐが明るい» のではないので、
    /// @brief 寿命だけを見ると跳ね返って減速した粒が «まだ若いから明るい» まま残る。
    /// @brief 正規化: speed / speedRange を 0..1 にクランプしてカーブを引く。
    float speedRange = 10.0f;
    bool useSpeedSizeCurve = false;
    ParticleCurve speedSizeCurve;         ///< @brief size への倍率
    bool useSpeedColorGradient = false;
    ParticleGradient speedColorGradient;  ///< @brief 寿命の色へ乗算する

    /// @brief 周回 (Vortex) と放射 (Source) は localForces に space = Emitter で入る。
    /// @brief 発生時にエミッター自身の移動速度を初速へ加算する割合 [0,1]。
    /// @brief 移動する剣・ロケットから出る火花が置き去りにならず、引きずられて見えるようになる。
    float inheritVelocity = 0.0f;

    /// @brief Emission拡張: 移動距離、Prewarm、時刻指定Burst。
    float rateOverDistance = 0.0f;
    bool prewarm = false;
    std::vector<ParticleBurst> bursts;

    /// @brief 履歴点へビルボードを並べるのではなく、連続した 1 枚の帯として描く。
    /// @brief ビルボード方式は太くすると粒の連なりが露見するので、幅のある帯が主役の表現
    /// @brief (剣閃・魔法の軌跡) には使えない。有効時は Trail ノードと同じリボン生成を通す。
    /// @brief 帯は 1 エミッターを 1 DrawCall で描くので、色は colorStart / colorEnd を長さ方向へ配る
    /// @brief (粒子ごとの色ゆらぎを尾へ乗せたいならビルボード方式のまま)。
    /// @brief SubEmitterはGameObject名で参照し、各イベントで対象EmitterへBurstを積む。

    ParticleCullingSettings culling;

    /// @brief 乱流 (Curl) と焼いた場 (Baked) も localForces に入る。
    /// @brief シーン内の FlowField から流れを受けるか。
    /// @note UI 演出用パーティクルなど、環境の流れに反応させたくないエミッターを除外できるようにする。
    bool receiveFlowFields = true;

    /// @brief 受け取る場を選ぶビットマスク。FlowField::channels と 1 ビットでも
    /// @brief 重なった場だけがこのエミッターに作用する。receiveFlowFields が false なら無関係。
    /// @brief 既定は全ビット ON で、従来どおり全部の場を受け取る。
    uint32_t flowFieldChannels = 0xFFFFFFFFu;
    /// @}



    /// @name 任意機能 (既定のまま使わないことが多い)
    /// @{
    ParticleCollisionMode collisionMode = ParticleCollisionMode::None;
    ParticleCollisionResponse collisionResponse = ParticleCollisionResponse::Bounce;
    float collisionRadius = 0.05f;
    float collisionBounciness = 0.5f;
    float collisionDamping = 0.0f;
    float collisionPlaneY = 0.0f;
    /// @brief Physics 衝突で «当たってよい» コライダーのレイヤー集合 (ビット n = レイヤー n)。
    /// @brief 既定は全ビット ON で従来どおり全レイヤーに当たる。
    /// @note トリガーはマスクに関わらず常に素通しする。トリガーは «通過を検知する体積» で
    /// @note あって面ではないので、そこで跳ねると «見えない壁で火花が止まる» になる。
    uint32_t collisionLayerMask = 0xFFFFFFFFu;
    /// @}
    /// @name 黒体放射 (色温度オーサリング)
    /// @{
    /// @brief 有効にすると colorGradient の RGB を温度カーブから毎フレーム作り直す
    /// @brief (アルファはグラデーション側の値をそのまま使う)。
    /// @brief 炎・爆発の色はすす粒子の温度による黒体放射で決まるので、任意の RGB を並べても
    /// @brief 炎に見えない。輻射輝度は T^4 に比例し、根元と先端の差は色差ではなく輝度差として出る。
    bool  blackbodyEnabled = false;
    /// @brief 寿命 [0,1] → 色温度 [K]。既定は焚き火の実測域 (根元 1900K → 先端 1100K)。
    /// @note ParticleCurve の既定キーは 0→1 なので、そのまま温度に使うと 1K = 真っ黒になる。
    /// @note 有効化した瞬間に炎らしい値が出ないと機能に気付けない。
    ParticleCurve temperatureCurve{
        {{ {0.0f, 1900.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f},
           {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f} }},
        2, ParticleCurveInterpolation::Linear };
    float blackbodyReferenceTemperature = 1800.0f; ///< @brief ここで intensity 倍の明るさになる
    float blackbodyIntensity = 1.0f;
    ParticleTrailSettings trail;
    ParticleLightSettings light;
    int subEmitterBurstCount = 1;
    /// @brief 発火元の粒子の速度をこのエミッターの初速へ継ぐ割合 [0,1]。既定 0 = 継がない。
    /// @note 既定 0 は、全部継ぐと火花が発火元と同じ向きに流れる «二番煎じ» になるため。
    /// @note 当たりの火花は 0.1〜0.3 程度、煙のように流れを見せたいときだけ大きくする。
    /// @note 効くのは注入されたスポーン (birth/death/collision) だけ。エミッター自身の移動を
    /// @note 継ぐのは inheritVelocity で別の値。
    float subEmitterInheritVelocity = 0.0f;
    /// @brief 名前引きの探索範囲を限定するルート GameObject。INVALID でシーン全体。
    /// @brief 同じ .vfx を複数配置すると同名の GO が並ぶので、シーン全体で引くと隣のインスタンスを
    /// @brief 吹かせてしまう。VFXSystem が頭出し (ResetSubtree) のたびに配下へ VFX ルートを配る。
    /// @brief シーンへ手で置いた Emitter は INVALID のままで、従来どおりシーン全体から引く。
    /// @brief ランタイム専用。シーン保存対象ではない。
    EntityID subEmitterScopeRoot = EntityID::INVALID;
    std::string birthSubEmitter;
    std::string deathSubEmitter;
    std::string collisionSubEmitter;
    /// @}

    /// @name 内蔵の流れを «役割» で引く
    /// @{
    /// @note 添字ではなく型で引く: プリセットもスクリプトも «乱れ» «渦» を触りたいのであって、
    /// @note リストの何番目かは知らない。同じ型が複数あれば先頭を返す。

    [[nodiscard]] const FlowFieldSettings* FindLocalForce(FlowFieldType type) const
    {
        for (const auto& force : localForces)
            if (force.fieldType == type) return &force;
        return nullptr;
    }

    [[nodiscard]] FlowFieldSettings* FindLocalForce(FlowFieldType type)
    {
        for (auto& force : localForces)
            if (force.fieldType == type) return &force;
        return nullptr;
    }

    /// @brief 無ければ既定値で 1 本足して返す。space は型ごとに «その流れが意味を持つ座標系» へ倒す。
    FlowFieldSettings& EnsureLocalForce(FlowFieldType type)
    {
        if (FlowFieldSettings* existing = FindLocalForce(type)) return *existing;
        FlowFieldSettings force;
        force.fieldType = type;
        /// @note 内蔵の流れは既定で «エミッター全体に効く»
        force.radius    = 0.0f;
        /// @note 周回と湧き出し・吸い込みはエミッター原点が要る。それ以外は世界の性質なので World。
        force.space = (type == FlowFieldType::Vortex
                    || type == FlowFieldType::Source
                    || type == FlowFieldType::Sink)
                        ? FlowFieldSpace::Emitter
                        : FlowFieldSpace::World;
        if (type == FlowFieldType::Curl)
            /// @note 足しただけで挙動が変わらないように
            force.strength = 0.0f;
        localForces.push_back(force);
        return localForces.back();
    }

    void RemoveLocalForce(FlowFieldType type)
    {
        localForces.erase(
            std::remove_if(localForces.begin(), localForces.end(),
                           [type](const FlowFieldSettings& f) { return f.fieldType == type; }),
            localForces.end());
    }

    /// @brief 重力加速度 [m/s^2]。
    /// @note 場だった頃の呼び出し側 (プリセット / ScriptProxy / Inspector) を残すための別名。
    [[nodiscard]] math::Vector3 GravityAcceleration() const { return gravity; }
    void SetGravityAcceleration(const math::Vector3& acceleration) { gravity = acceleration; }

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
        /// @note IReflector は uint32_t 非対応のため int 経由で編集・保存する。
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
        r.Field("randomStartRotation", randomStartRotation);
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
        r.Field("receiveFlowFields", receiveFlowFields);
        /// @note IReflector は符号なし整数を扱わない。ビットパターンは int 経由でも保たれる。
        int channelsValue = static_cast<int>(flowFieldChannels);
        r.Field("flowFieldChannels", channelsValue);
        flowFieldChannels = static_cast<uint32_t>(channelsValue);
        /// @note 粒子だけの加速度と、流れへの結合。どちらも «場» から出した値。
        r.Field("gravity", gravity);
        r.Field("flowCoupling", flowCoupling);

        /// @name ここから下は以前 Reflect に無く、コーデックにしか居なかった 44 項目
        /// @note シーン保存はコーデック経由だが、AI バス (EditorBusDispatcher) と汎用 Inspector は
        /// @note Reflect() を使う。片方にしかないと「保存されるが外から読めない」まま気付けない。

        /// @note 速度による見た目
        r.Field("speedRange", speedRange);
        r.Field("useSpeedSizeCurve", useSpeedSizeCurve);
        r.Field("useSpeedColorGradient", useSpeedColorGradient);
        r.Field("useRotationCurve", useRotationCurve);
        r.Field("useDragCurve", useDragCurve);
        r.Field("useEmitRateCurve", useEmitRateCurve);

        /// @note 衝突
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
        /// @note flowFieldChannels と同じ理由で int 経由 (IReflector は符号なし整数を扱わない)。
        int collisionLayersValue = static_cast<int>(collisionLayerMask);
        r.Field("collisionLayerMask", collisionLayersValue);
        collisionLayerMask = static_cast<uint32_t>(collisionLayersValue);

        /// @note 黒体放射
        r.Field("blackbodyEnabled", blackbodyEnabled);
        r.Field("blackbodyReferenceTemperature", blackbodyReferenceTemperature);
        r.Field("blackbodyIntensity", blackbodyIntensity);

        /// @note per-particle トレイル
        r.Field("trailEnabled", trail.trailEnabled);
        r.Field("trailPointCount", trail.trailPointCount);
        r.Field("trailSampleInterval", trail.trailSampleInterval);
        r.Field("trailWidthScale", trail.trailWidthScale);
        r.Field("trailAlphaScale", trail.trailAlphaScale);
        r.ColorField("trailColorTint", trail.trailColorTint);
        r.Field("trailRibbon", trail.trailRibbon);
        r.Field("trailRibbonWidth", trail.trailRibbonWidth);

        /// @note 粒子を点光源にする
        r.Field("lightEnabled", light.lightEnabled);
        r.Field("lightRatio", light.lightRatio);
        r.Field("lightMaxCount", light.lightMaxCount);
        r.Field("lightRange", light.lightRange);
        r.Field("lightRangeFromSize", light.lightRangeFromSize);
        r.Field("lightIntensity", light.lightIntensity);
        r.Field("lightUseParticleColor", light.lightUseParticleColor);
        r.ColorField("lightColor", light.lightColor);
        r.Field("lightFadeWithAlpha", light.lightFadeWithAlpha);

        /// @note サブエミッター (scopeRoot はランタイム専用なので出さない)
        r.Field("birthSubEmitter", birthSubEmitter);
        r.Field("deathSubEmitter", deathSubEmitter);
        r.Field("collisionSubEmitter", collisionSubEmitter);
        r.Field("subEmitterBurstCount", subEmitterBurstCount);
        r.Field("subEmitterInheritVelocity", subEmitterInheritVelocity);

        /// @note カーブとグラデーション。IReflector は専用の仮想関数を持っているので、
        /// @note コーデックの手書きと同じ形をそのまま表現できる。
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
    /// @}
};

/// @brief runtimeGradient を作り直す。入力が変わっていなければ何もしないので、どこから呼んでもよい。
/// @note 黒体モードでは各キーの時刻で温度カーブを引き、色温度 → リニア RGB → オーサリング空間
/// @note (sRGB) へ戻して格納する。オーサリング空間で持つのは、CPU 経路も GPU 経路も
/// @note 「補間 → リニア化」という同じ順序を通すため。ここだけ別空間にすると片方が破綻する。
/// @note 自由関数にしたのは、焼き込みが settings→runtime の純粋な変換で済むため。実体を持たない
/// @note VFX グラフのノードでも、Inspector が焼き込み後の色を出せる。
inline void RefreshParticleRuntimeGradient(const ParticleEmitterSettings& settings, ParticleRuntime& runtime)
{
    /// @note 黒体の焼き込みはキー 1 点あたり可視域 81 サンプルの積分になる。粒子ごとの
    /// @note 更新から間接的に呼ばれても潰れないよう、入力が変わったときだけ作り直す。
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
    /// @brief オーサリング設定。VFXGraphNode などが持つ塊と同じ型。
    ParticleEmitterSettings settings;
    /// @brief 実行中にしか意味を持たない状態。シーンにも .particle にも保存しない。
    ParticleRuntime runtime;

    void RefreshRuntimeGradient() { RefreshParticleRuntimeGradient(settings, runtime); }

    /// @brief 実効プレビュー速度。エディターからの書き込みが 2 フレーム以上途絶えていたら 1.0 に戻す。
    /// @note ゲーム実行時 (エディターなし) は書き込みが存在しないため常に 1.0 になり、影響しない。
    float GetEditorTimeScale(uint64_t currentFrame) const
    {
        return (runtime.editorTimeScaleFrame + 2 >= currentFrame) ? runtime.editorTimeScale : 1.0f;
    }

    /// @brief 再生状態と粒子を先頭へ巻き戻す。Inspector / VFX Editor の Restart とスクラブ前処理が共用する。
    /// @note リセットすべきランタイム状態が多く、呼び出し側ごとに列挙すると漏れが出るため一箇所に集約する。
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
        /// @note ロード直後やスクラブ開始時に、再生が 1 フレームも進まないまま粒子色を
        /// @note 引かれることがある。実効グラデーションを先に用意しておく。
        RefreshRuntimeGradient();
    }

    /// @brief Inspector / Script / Operator が共有する再生制御。
    /// @note UI と ScriptProxy がそれぞれランタイム状態を列挙すると、Clear や Restart の
    /// @note 対象漏れが経路ごとに発生する。エミッター自身に責務を集め、AI も同じ挙動を使う。
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

    /// @brief 設定へ転送する。保存対象は settings の中身と一致するので、正本は 1 つで済む。
    void Reflect(IReflector& r)
    {
        const uint32_t seedBefore = settings.randomSeed;
        settings.Reflect(r);
        /// @note 種を編集したら乱数列も追従させる。Reflect は Inspector / AI バスの入口なので、
        /// @note ここで揃えないと「種を変えたのに絵が変わらない」になる。
        if (settings.randomSeed != seedBefore) runtime.randomState = settings.randomSeed;
    }
};

} /// @note namespace fbzz::scene
