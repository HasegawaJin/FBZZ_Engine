/// @file    EnemyDeathVfxComponent.hpp
/// @brief   撃破された敵の「体そのものが粒になって立ち昇る」演出
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 体の «形» から出すか:
///   撃破の演出を «その場に爆発を 1 発置く» で済ませると、どの敵が倒れたのかは
///   位置でしか読めない。倒れた瞬間に一番見たいのは «さっきまで動いていた形» で、
///   そこから粒が湧けば、爆発を置かなくても「あれが消えた」と分かる。
///   ParticleEmitterShape::MeshSurface が現在のスキンポーズごと発生点をくれるので、
///   死亡モーションの途中の姿勢からそのまま粒が出る。
///
/// WHY 法線方向へ初速を与えるか:
///   表面から出すだけでは、全粒子が同じ向きに飛ぶ «形をした噴水» になる。
///   法線へ小さく押し出して即座に減衰させると、面から «にじみ出て» から上へ
///   流れ始める。この «一度膨らんでから昇る» が撃破演出の骨格になっている。
///
/// WHY 体を «溶かす» のに .mat を差し替えるか:
///   半透明にして薄れさせるだけでは «幽霊のように消えた» にしかならない。撃破は
///   物が壊れる出来事なので、面が食われて縁が灼ける必要がある。それはシェーダーの
///   仕事で、Assets/Shaders/Material/Skinned/SkinnedEnemyDissolve.hlsl が受け持つ。
///   差し替えは MaterialInstance (GameObject 単位) で行うので、同じ .mat を使っている
///   生きている敵には波及しない。
///
/// WHY 差し替え先を規約で引くか:
///   敵 1 体は部位ごとに別 GameObject / 別 .mat で描かれている (Roller は 7 枚)。
///   Inspector に 7 本の参照を並べると、部位を 1 つ足すたびにシーンを触ることになる。
///   «今その部位に付いている .mat の名前 + _Dissolve» を
///   Assets/Materials/Enemies/<キャラ>/ から引けば、結線は 0 本で済む。
///   見つからない部位は半透明フェードへ落ちるので、片方だけ用意しても壊れない。
///
/// WHY 画面演出 (ヒットストップ・カメラ揺れ) を持たないか:
///   VfxManagerComponent と同じ理由で、配分は ImpactFeedbackManagerComponent が持つ。
///   ここは «その場所に何が見えるか» だけを受け持つ。
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 粒の既定素材。VfxManagerComponent と同じで、未割り当てでも «何も出ない» にはしない。
inline constexpr const char* kDeathMoteMaterial =
    "Assets/Materials/Effects/FX_GlowSoft_Additive.mat";

// ディゾルブ .mat の置き場と命名規約。<root>/<キャラ>/<元の .mat 名>_Dissolve.mat
inline constexpr const char* kDissolveMaterialRoot = "Assets/Materials/Enemies";
inline constexpr const char* kDissolveMaterialSuffix = "_Dissolve";

// SkinnedEnemyDissolve.hlsl が公開する «食われ具合»。0 = 無傷、1 = 消滅。
// 綴りはシェーダーの cbuffer 変数名と一致していること (GlowMaterial.hpp と同じ理由で、
// ずれると MaterialInstance が書き込みを黙って捨てる)。
inline constexpr MaterialPropertyId kDissolveAmountId{ "dissolveAmount" };
inline constexpr MaterialPropertyId kAlbedoId{ "albedo" };

class EnemyDeathVfxComponent : public Script {
    FBZZ_SCRIPT(EnemyDeathVfxComponent)

public:
    FBZZ_GROUP("Motes")
    FBZZ_ASSET_FIELD(MaterialRef, moteMaterial, "Material")
    FBZZ_TOOLTIP("未割り当てなら FX_GlowSoft_Additive.mat を使う")
    FBZZ_FIELD_RANGE_INT(int, burstCount, 240, "バースト", 0, 4000)
    FBZZ_TOOLTIP("倒れた瞬間に一度に湧く数。輪郭が «一瞬で粒になる» 読みを作る")
    FBZZ_FIELD_RANGE(float, emitRate, 380.0f, "発生レート", 0.0f, 4000.0f)
    FBZZ_TOOLTIP("その後こぼれ続ける量 [個/秒]")
    FBZZ_FIELD_RANGE(float, emitSeconds, 0.55f, "Emit Seconds", 0.0f, 4.0f)
    FBZZ_TOOLTIP("こぼれ続ける時間。体が消えるより先に止める")

    FBZZ_GROUP("動き")
    FBZZ_FIELD_RANGE(float, normalVelocity, 0.65f, "Normal Velocity", -10.0f, 10.0f)
    FBZZ_TOOLTIP("表面法線へ押し出す初速 [m/s]。負値は体へ吸い込む")
    FBZZ_FIELD_RANGE(float, riseAcceleration, 2.3f, "立ち上がり", -20.0f, 20.0f)
    FBZZ_TOOLTIP("上向きの加速度 [m/s^2]。法線の勢いが切れた後の «昇り» を作る")
    FBZZ_FIELD_RANGE(float, spread, 0.30f, "拡がり", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, damping, 3.4f, "減衰", 0.0f, 20.0f)
    FBZZ_TOOLTIP("法線の押し出しを早く殺すほど «膨らんでから昇る» が強く出る")
    FBZZ_FIELD_RANGE(float, turbulence, 0.85f, "Turbulence", 0.0f, 10.0f)

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(colorStart, (Vector4{ 0.52f, 1.00f, 0.72f, 1.00f }), "Start")
    FBZZ_FIELD_COLOR(colorEnd,   (Vector4{ 0.05f, 0.26f, 0.19f, 0.00f }), "End")
    FBZZ_FIELD_RANGE(float, sizeStart, 0.085f, "Size Start", 0.001f, 2.0f)
    FBZZ_FIELD_RANGE(float, sizeEnd,   0.010f, "Size End",   0.000f, 2.0f)
    // WHY moteLifetime か: Script は lifetime という名前のプロキシを持っている。
    //     同じ名前のフィールドを足すと基底が隠れ、この先 lifetime.* を書いた誰かが
    //     «なぜかコンパイルが通らない» に当たる。
    FBZZ_FIELD_RANGE(float, moteLifetime, 1.40f, "寿命", 0.05f, 8.0f)
    FBZZ_FIELD_RANGE(float, moteLifetimeRandom, 0.45f, "Lifetime Random", 0.0f, 1.0f)

    FBZZ_GROUP("Body Dissolve")
    FBZZ_FIELD_RANGE(float, fadeDelay, 0.22f, "遅延", 0.0f, 4.0f)
    FBZZ_TOOLTIP("倒れてから体が崩れ始めるまで。0 だと死亡モーションが読めない")
    FBZZ_FIELD_RANGE(float, fadeSeconds, 0.60f, "継続時間", 0.01f, 4.0f)
    FBZZ_FIELD(std::string, dissolveFolder, "", "Material Folder")
    FBZZ_TOOLTIP("ディゾルブ .mat の置き場。空ならモデルのフォルダ名から "
                 "Assets/Materials/Enemies/<キャラ> を組み立てる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugModel, "", "Shape Model")
    FBZZ_FIELD_READ_ONLY(std::string, debugDissolveFolder, "", "Dissolve Folder")
    FBZZ_FIELD_READ_ONLY(int, debugBodyParts, 0, "Body Parts")
    FBZZ_FIELD_READ_ONLY(int, debugDissolveParts, 0, "Dissolving Parts")

    /// 撃破された瞬間に 1 度だけ呼ぶ。2 度目以降は無視する。
    void Begin();

    /// 演出が終わるまでの秒数。撃破側はこれ以上待ってから GameObject を畳むこと。
    /// WHY: 粒はエミッターが持っているので、体を先に消すと出ている粒ごと消える。
    [[nodiscard]] float TotalSeconds() const;

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// 崩れさせる 1 スロット。
    /// dissolvePath が空でなければディゾルブへ差し替えられる部位で、そうでなければ
    /// 半透明フェードへ落ちる。baseAlbedo はそのフェード用 (上書きは «色まるごと»
    /// の単位でしか書けないので、RGB を保つために開始時の値が要る)。
    struct BodySlot {
        EntityRef   target;
        uint32_t    slot = 0;
        Vector4     baseAlbedo{ 1.0f, 1.0f, 1.0f, 1.0f };
        std::string dissolvePath;
    };

    void CollectBody(GameObject& object);
    void ConfigureEmitter(ParticleEmitterSettings& emitter) const;
    [[nodiscard]] std::string MoteMaterialPath() const;
    /// ディゾルブ .mat の置き場。Inspector 指定が空ならモデルのフォルダ名から組む。
    [[nodiscard]] std::string ResolveDissolveFolder() const;
    /// 共有 .mat のパス → 同じ部位のディゾルブ版。規約に載らなければ空文字列。
    [[nodiscard]] std::string DissolvePathFor(const std::string& sharedPath) const;

    std::vector<BodySlot> m_body;
    std::string m_modelPath;
    std::string m_dissolveFolder;
    float m_elapsed = 0.0f;
    bool  m_running = false;
};

FBZZ_REFLECT(EnemyDeathVfxComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline std::string EnemyDeathVfxComponent::MoteMaterialPath() const
{
    std::string path = moteMaterial.ResolvePath();
    return path.empty() ? std::string(kDeathMoteMaterial) : path;
}

inline std::string EnemyDeathVfxComponent::ResolveDissolveFolder() const
{
    if (!dissolveFolder.empty()) return dissolveFolder;
    if (m_modelPath.empty()) return {};

    // "Assets/Models/Enemy_Mite/Z_Mite.fbx" → "Mite"
    const std::size_t fileStart = m_modelPath.find_last_of("/\\");
    if (fileStart == std::string::npos || fileStart == 0) return {};
    const std::size_t dirStart = m_modelPath.find_last_of("/\\", fileStart - 1);
    std::string character = dirStart == std::string::npos
        ? m_modelPath.substr(0, fileStart)
        : m_modelPath.substr(dirStart + 1, fileStart - dirStart - 1);

    // モデルのフォルダは "Enemy_Mite" だが、材質のフォルダは "Mite" にする。
    // 材質側にまで Enemy_ を繰り返すと Materials/Enemies/Enemy_Mite になって読みにくい。
    constexpr std::string_view kEnemyPrefix = "Enemy_";
    if (character.compare(0, kEnemyPrefix.size(), kEnemyPrefix) == 0)
        character.erase(0, kEnemyPrefix.size());
    if (character.empty()) return {};

    return std::string(kDissolveMaterialRoot) + "/" + character;
}

inline std::string EnemyDeathVfxComponent::DissolvePathFor(const std::string& sharedPath) const
{
    if (m_dissolveFolder.empty() || sharedPath.empty()) return {};

    const std::size_t start = sharedPath.find_last_of("/\\");
    const std::size_t nameStart = start == std::string::npos ? 0 : start + 1;
    const std::size_t dot = sharedPath.find_last_of('.');
    const std::size_t nameEnd = (dot == std::string::npos || dot < nameStart)
        ? sharedPath.size() : dot;

    const std::string stem = sharedPath.substr(nameStart, nameEnd - nameStart);
    if (stem.empty()) return {};
    return m_dissolveFolder + "/" + stem + kDissolveMaterialSuffix + ".mat";
}

inline void EnemyDeathVfxComponent::OnStart()
{
    m_body.clear();
    m_modelPath.clear();
    m_dissolveFolder.clear();
    m_elapsed  = 0.0f;
    m_running  = false;

    // 形状の解決が先。ディゾルブの置き場はモデルのフォルダ名から組み立てる。
    // 差し替え先そのものは Begin() まで引かない — 部位の材質は生きているあいだに
    // 別のものへ差し替えられうるので、«倒れた時点で付いているもの» を見る必要がある。
    if (GameObject* self = scene.Self()) CollectBody(*self);
    m_dissolveFolder = ResolveDissolveFolder();

    debugModel          = m_modelPath;
    debugDissolveFolder = m_dissolveFolder;
    debugBodyParts      = static_cast<int>(m_body.size());
    debugDissolveParts  = 0;

    if (m_modelPath.empty()) {
        debug.LogError("EnemyDeathVfxComponent found no SkinnedMeshRenderer under this object. "
                       "The death motes have no shape to spawn from.");
    }
}

inline void EnemyDeathVfxComponent::CollectBody(GameObject& object)
{
    // 形状は «この敵を描いているモデル» から引く。部位ごとに GameObject が分かれていても
    // modelPath は同じ 1 本なので、最初に見つけた 1 つでモデル全体 (meshIndex = -1) を指せる。
    if (const auto* skinned = object.GetComponent<SkinnedMeshRenderer>()) {
        if (m_modelPath.empty()) m_modelPath = skinned->modelPath;

        // 描いている submesh のぶんだけスロットがある。モデルがまだロードされていない
        // 段階でも 1 枚は見に行く (submeshIndices が空だと SubmeshCount が 0 を返す)。
        // 実在しないスロットは MaterialInstance が無効を返すので、そこで打ち切る。
        const uint32_t slotCount = skinned->SubmeshCount() > 0
            ? static_cast<uint32_t>(skinned->SubmeshCount()) : 1u;
        for (uint32_t slot = 0; slot < slotCount; ++slot) {
            BodySlot entry{ EntityRef{ object.GetID() }, slot, Vector4{ 1.0f, 1.0f, 1.0f, 1.0f } };
            const MaterialInstance instance = material.Instance(entry.target, slot);
            if (!instance.IsValid()) break;
            // 読めない (この .mat が albedo を持たない) 場合でも既定の白で続ける。
            // 消えていく途中の色より、«消える» ことの方が読みとして重要。
            (void)instance.TryGetColor(kAlbedoId, entry.baseAlbedo);
            m_body.push_back(entry);
        }
    }

    for (int i = 0; i < object.GetChildCount(); ++i)
        if (GameObject* child = object.GetChild(i)) CollectBody(*child);
}

inline float EnemyDeathVfxComponent::TotalSeconds() const
{
    const float motes = std::max(emitSeconds, 0.0f)
        + std::max(moteLifetime, 0.05f) * (1.0f + Clamp01(moteLifetimeRandom));
    const float fade = std::max(fadeDelay, 0.0f) + std::max(fadeSeconds, 0.01f);
    return std::max(motes, fade);
}

inline void EnemyDeathVfxComponent::ConfigureEmitter(ParticleEmitterSettings& emitter) const
{
    emitter.materialPath = MoteMaterialPath();

    emitter.shape                           = ParticleEmitterShape::MeshSurface;
    emitter.meshShapePath                   = m_modelPath;
    emitter.meshShapeIndex                  = -1;   // 体をまるごと使う
    emitter.meshShapeScale                  = 1.0f; // 縮尺は GameObject の worldScale が持つ
    emitter.meshShapeFollowSkinnedAnimation = true;
    emitter.meshShapeNormalVelocity         = normalVelocity;

    // WHY World か: 死亡モーションは体を大きく動かす。Local だと既に出ている粒まで
    //     その動きに引きずられ、«体から離れた» はずの粒が体と一緒に流れる。
    emitter.simulationSpace = ParticleSimulationSpace::World;
    emitter.emitPosition    = Vector3::ZERO;
    emitter.emitVelocity    = Vector3::ZERO;
    emitter.velocitySpread  = spread;
    // 法線の押し出しを早く殺し、代わりに上向きの加速度だけを残す。
    emitter.velocityDamping = damping;
    emitter.gravity         = { 0.0f, riseAcceleration, 0.0f };
    emitter.noiseStrength   = turbulence;
    emitter.noiseFrequency  = 0.8f;
    emitter.noiseSpeed      = 1.2f;

    emitter.lifetime       = moteLifetime;
    emitter.lifetimeRandom = moteLifetimeRandom;
    emitter.sizeStart      = sizeStart;
    emitter.sizeEnd        = sizeEnd;
    emitter.colorStart     = colorStart;
    emitter.colorEnd       = colorEnd;
    // 群れが 1 枚のベタ塗りに見えないだけの揺らぎ。白へ寄せないので加算でも飽和しない。
    emitter.colorVariation = 0.25f;

    emitter.emitRate = emitRate;
    // 一度に湧く分 + こぼれ続ける分。足りないと Burst の途中で打ち切られ、
    // 輪郭が «半分だけ粒になった» 形で出る。
    emitter.maxParticles = std::max(
        burstCount + static_cast<int>(emitRate * std::max(emitSeconds, 0.0f)) + 32, 32);

    // 加算合成なので並べ替えは要らない (前後関係が絵に出ない)。
    emitter.sortMode = ParticleSortMode::None;
    // 発生は emitSeconds で自然に止まる。clearOnStop は立てない —
    // 止まった瞬間に既に出ている粒まで消える。
    emitter.loop        = false;
    emitter.duration    = std::max(emitSeconds, 0.0f);
    emitter.clearOnStop = false;
    emitter.playing     = true;
    emitter.enabled     = true;
}

inline void EnemyDeathVfxComponent::Begin()
{
    if (m_running || m_modelPath.empty()) return;
    m_running = true;
    m_elapsed = 0.0f;

    // シーンで作り込んだエミッターがあればそれを使い、無ければ足す。
    ParticleEmitter& emitter = scene.GetOrAddComponent<ParticleEmitter>();
    ConfigureEmitter(emitter.settings);
    particle.Play(/*restart=*/true);
    particle.Burst(std::max(burstCount, 0));

    // 体をディゾルブ版へ差し替える。差し替えは GameObject 単位なので、
    // 同じ .mat を使っている生きている敵はそのまま無傷で残る。
    debugDissolveParts = 0;
    for (BodySlot& part : m_body) {
        part.dissolvePath = DissolvePathFor(
            material.GetSharedMaterialPath(part.target, part.slot));

        const MaterialInstance before = material.Instance(part.target, part.slot);
        // 発光色は GlowPartComponent が override で持っている。
        // SetSharedMaterial はパスが変わると override を全消しするので、差し替えを跨いで
        // 手で運ぶ。毎フレーム押し直す作りなので放っておいても翌フレームには戻るが、
        // その 1 フレームだけコアが黒く落ちるのが «撃破の瞬間» に重なって目に付く。
        Vector3 emissiveColor{ 1.0f, 1.0f, 1.0f };
        float   emissiveScale = 0.0f;
        const bool hadColor = before.TryGetVector3(kEmissiveColorId, emissiveColor);
        const bool hadScale = before.TryGetFloat(kEmissiveScaleId, emissiveScale);

        if (!part.dissolvePath.empty()
            && material.SetSharedMaterial(part.target, MaterialRef(part.dissolvePath), part.slot)) {
            const MaterialInstance after = material.Instance(part.target, part.slot);
            if (hadColor) (void)after.SetVector3(kEmissiveColorId, emissiveColor);
            if (hadScale) (void)after.SetFloat(kEmissiveScaleId, emissiveScale);
            ++debugDissolveParts;
            continue;
        }

        // ディゾルブ版が無い / 読めない部位は半透明フェードへ落とす。
        // 片方だけ用意した状態でも «その部位だけ最後まで残る» にはしない。
        part.dissolvePath.clear();
        const MaterialInstance instance = material.Instance(part.target, part.slot);
        (void)instance.SetBlendMode(MaterialBlendMode::Alpha);
    }
}

inline void EnemyDeathVfxComponent::OnUpdate()
{
    if (!m_running) return;
    m_elapsed += Time::deltaTime;

    const float fadeT = Clamp01((m_elapsed - std::max(fadeDelay, 0.0f))
                                / std::max(fadeSeconds, 0.01f));
    // 二乗で落とす。線形だと «最後まで薄く残っている» 時間が長く、粒が消えた後に
    // 輪郭の亡霊だけが立っているように見える。ディゾルブ側は面が実際に欠けるので
    // この補正は要らない (掛けると崩れ始めが遅れて «一気に消えた» になる)。
    const float alpha = (1.0f - fadeT) * (1.0f - fadeT);

    for (const BodySlot& part : m_body) {
        const MaterialInstance instance = material.Instance(part.target, part.slot);
        if (!part.dissolvePath.empty()) {
            (void)instance.SetFloat(kDissolveAmountId, fadeT);
            continue;
        }
        (void)instance.SetColor(kAlbedoId,
                                Vector4{ part.baseAlbedo.x, part.baseAlbedo.y,
                                         part.baseAlbedo.z, part.baseAlbedo.w * alpha });
    }
}

} // namespace sandbox
