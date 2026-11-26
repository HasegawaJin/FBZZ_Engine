/// @file    RenderParticleInput.hpp
/// @brief   パーティクル描画と GPU 更新要求の不変入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <Graphics/Effects/Particles/ParticleDrawTypes.hpp>
namespace fbzz::renderer {
struct ParticleDrawSettings {
    ParticleSortMode sortMode = ParticleSortMode::None; ///< ビューごとに粒子を並べる基準。
    ParticleSimulationSpace simulationSpace = ParticleSimulationSpace::World; ///< 粒子の位置・速度を格納する座標系。
    ParticleTrailSettings trail; ///< 粒子が残す軌跡の描画設定。
    math::Vector4 colorStart{}; ///< 寿命の開始時に使う粒子色。
    math::Vector4 colorEnd{}; ///< 寿命の終端で使う粒子色。
    bool meshParticle = false; ///< 粒子の形状にメッシュを使うかどうか。
    int maxParticles = 1; ///< エミッターの描画とシミュレーションで扱う粒子の上限。
};
struct ParticleDrawMaterial {
    bool distortion = false; ///< 背景色を読む歪み描画が必要かどうか。
    bool punctualLighting = false; ///< 点光源・スポット光源を材質で評価するかどうか。
    float selfShadowStrength = 0; ///< 粒子材質に適用する自己遮蔽の強さ。0 で無効。
};
struct ParticleDrawResources {
    ResourceHandle<TextureTag> texture; ///< 粒子の基準色を読むテクスチャ。
    ResourceHandle<TextureTag> distortionTexture; ///< 背景画像の参照位置をずらす歪みテクスチャ。
    ResourceHandle<TextureTag> motionVectorTexture; ///< コマ間の補間に使うモーションベクトルテクスチャ。
    ResourceHandle<TextureTag> sixWayNegativeTexture; ///< 6 方向ライティングで負軸側の応答を読むテクスチャ。
    ResourceHandle<TextureTag> sixWayAlbedoColorTexture; ///< 6 方向ライティングで基準色を上書きするテクスチャ。
    ResourceHandle<TextureTag> sixWayEmissionColorTexture; ///< 6 方向ライティングで自発光色を読むテクスチャ。
    ResourceHandle<ShaderTag> customShader; ///< 材質が置き換える描画シェーダー。無効なら既定経路を使う。
    ResourceHandle<ConstantBufferTag> renderCB; ///< 粒子の描画定数を書き込むバッファ。
    ResourceHandle<ConstantBufferTag> materialParamsCB; ///< 材質固有の反映済みパラメーターを読むバッファ。
    ResourceHandle<ConstantBufferTag> trailRibbonCB; ///< 軌跡リボンの描画定数を書き込むバッファ。
    ResourceHandle<ConstantBufferTag> gpuEmitterCB; ///< GPU 更新のエミッター定数を書き込むバッファ。
    ResourceHandle<ConstantBufferTag> gpuSortCB; ///< GPU 並べ替え用のビューと粒子数を渡すバッファ。
    ResourceHandle<StructuredBufferTag> gpuParticleBuffer; ///< GPU 更新と描画が共有する粒子状態。
    ResourceHandle<StructuredBufferTag> gpuSpawnBuffer; ///< GPU 更新へ渡す新規粒子の入力列。
    ResourceHandle<StructuredBufferTag> gpuForceBuffer; ///< GPU 更新へ渡す流れ場の入力列。
    ResourceHandle<StructuredBufferTag> gpuSortBuffer; ///< ビューごとの描画順を GPU で計算するバッファ。
    uint32_t gpuSortCapacity = 0; ///< gpuSortBuffer に書き込める要素数。
    ParticleDrawMaterial material; ///< 解決済みの描画材質フラグ。
    ParticleBlendMode resolvedBlend = ParticleBlendMode::Additive; ///< 描画で使う解決済みブレンド方式。
    bool textureIsSrgb = true; ///< 基準色テクスチャを sRGB として扱うかどうか。
    std::vector<Particle> particles; ///< CPU 経路で描く、抽出済みの粒子状態。
};
struct RenderParticleInput {
    uint32_t layer = 0; ///< ビューのカリングマスクに照合するレイヤー番号。
    int priority = 0; ///< 複数エミッターを描く順序の優先度。
    int drawCount = 0; ///< エミッターがこのフレームに描画する粒子数。
    math::Vector3 position; ///< ワールド空間のエミッター位置 [m]。
    math::Vector3 boundsCenter; ///< フラスタム判定に使うワールド空間の境界球中心 [m]。
    float boundsRadius = 0; ///< 境界球の半径 [m]。
    bool gpu = false; ///< GPU に粒子状態を保持する経路を使うかどうか。
    bool simulate = false; ///< このフレームに GPU 更新を要求するかどうか。
    ParticleDrawSettings settings; ///< 描画と並べ替えに使う解決済み設定。
    ParticleDrawResources runtime; ///< 粒子のスナップショットと既存 GPU バッファの参照。
    ParticleRenderCB constants{}; ///< 粒子描画シェーダーへ渡すエミッター固有定数。
    GpuParticleEmitterCB simulation{}; ///< GPU 更新時に渡す時間差、スポーン、運動設定。
    ResourceHandle<TextureTag> velocityField; ///< GPU 更新時に試料を取る速度場のテクスチャ。
    ResourceHandle<BufferTag> meshVertices; ///< メッシュ粒子の頂点バッファ。
    ResourceHandle<BufferTag> meshIndices; ///< メッシュ粒子のインデックスバッファ。
    uint32_t meshIndexCount = 0; ///< 1 粒子で読む meshIndices の要素数。
    uint32_t meshVertexCount = 0; ///< メッシュ粒子の頂点数。
};
} // namespace fbzz::renderer
