/// @file    VFXSystem.cpp
/// @brief   VFX ルートの時刻進行、生存窓の開閉、エンベロープの適用、終了時の片付け。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/Systems/VFXSystem.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VFXAudioEnvelope.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/VFXElement.hpp>
#include <Engine/Scene/Components/VFXScreenEffect.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabPool.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/AudioSystem.hpp>
#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene {
namespace {

constexpr float kMinSpan = 0.0001f;

/// @brief 尺を算出できなかった VFX に与える既定の尺 [秒]。
/// @note 0 は「無限」として扱うため、0 のままだと autoDestroy な実体が二度と片付かない。
///       何も置かれていない空の VFX で漏らすより、1 秒で畳んで «出ていない» と気付ける方がよい。
constexpr float kFallbackDuration = 1.0f;

/// 階層の走査

/// root 配下を訪ねる。入れ子 VFX の内側へは降りない (その VFXComponent が自分で回す)。
template<typename Fn>
void ForEachDescendant(GameObject& root, Fn&& fn)
{
    const int count = root.GetChildCount();
    for (int index = 0; index < count; ++index) {
        GameObject* child = root.GetChild(index);
        if (child == nullptr) continue;
        fn(*child);
        if (child->GetComponent<VFXComponent>() != nullptr) continue;
        ForEachDescendant(*child, fn);
    }
}

/// 祖先に VFXComponent があるか。入れ子ルートは autoDestroy を持たない。
[[nodiscard]] bool HasAncestorVFX(GameObject& gameObject)
{
    for (GameObject* parent = gameObject.GetParent(); parent != nullptr;
         parent = parent->GetParent()) {
        if (parent->GetComponent<VFXComponent>() != nullptr) return true;
    }
    return false;
}

/// 尺の算出

/// このオブジェクトが要求する終了時刻 [秒]。
/// 無限に続くもの (loop) は outEndless を立てて 0 を返す。
[[nodiscard]] float ObjectEndTime(GameObject& gameObject, bool& outEndless)
{
    float end = 0.0f;

    if (auto* element = gameObject.GetComponent<VFXElement>(); element != nullptr && element->enabled) {
        /// @note duration <= 0 は「ルートが終わるまで」の意味なので、尺そのものは要求しない。
        ///       ここで無限にすると «自分の長さを自分で決める» 循環になる。
        if (element->loop) outEndless = true;
        else if (element->duration > 0.0f)
            end = (std::max)(end, element->startDelay + element->duration);
    }

    if (auto* emitter = gameObject.GetComponent<ParticleEmitter>()) {
        const ParticleEmitterSettings& s = emitter->settings;
        if (s.loop) outEndless = true;
        else {
            /// @note 粒子は発生が終わってからも寿命ぶん生き残る。emitRate/duration だけで
            ///       畳むと «最後のひと粒が出た瞬間に消える» ことになる。
            const float lifetime = s.lifetime * (1.0f + (std::max)(s.lifetimeRandom, 0.0f));
            end = (std::max)(end, s.startDelay + s.duration + lifetime);
        }
    }

    if (auto* trail = gameObject.GetComponent<TrailComponent>())
        end = (std::max)(end, trail->duration);

    if (auto* decal = gameObject.GetComponent<DecalComponent>()) {
        if (decal->lifetime < 0.0f) outEndless = true;
        else end = (std::max)(end, decal->lifetime);
    }

    return end;
}

/// 配下から VFX 全体の尺を求める。
[[nodiscard]] float ResolveDuration(GameObject& root, bool& outEndless)
{
    float longest = 0.0f;
    outEndless = false;
    ForEachDescendant(root, [&](GameObject& child) {
        longest = (std::max)(longest, ObjectEndTime(child, outEndless));
    });
    if (outEndless) return 0.0f;
    return longest > 0.0f ? longest : kFallbackDuration;
}

/// 生存窓の開閉

/// 窓を開いた瞬間の頭出し。
void RewindPlayback(GameObject& gameObject)
{
    if (auto* screen = gameObject.GetComponent<VFXScreenEffect>()) {
        screen->enabled = true;
        /// @note 立ち上がりは weightCurve に任せる
        screen->weight = 0.0f;
        screen->progress = 0.0f;
    }
    if (auto* shake = gameObject.GetComponent<VFXCameraShake>()) {
        shake->enabled = true;
        shake->weight = 0.0f;
        shake->elapsed = 0.0f;
    }
    if (auto* scale = gameObject.GetComponent<VFXTimeScale>()) {
        scale->enabled = true;
        scale->weight = 0.0f;
    }
    if (auto* emitter = gameObject.GetComponent<ParticleEmitter>()) emitter->ResetPlayback();
    if (auto* decal = gameObject.GetComponent<DecalComponent>()) {
        decal->age = 0.0f;
        decal->opacity = 1.0f;
    }
    if (auto* animator = gameObject.GetComponent<AnimatorComponent>()) {
        animator->stateTime = 0.0f;
        animator->previousEventTime = 0.0f;
        animator->previousEventClipName.clear();
        animator->firedEvents.clear();
    }
    if (auto* audio = gameObject.GetComponent<AudioSourceComponent>()) {
        audio->m_played = false;
        audio->m_pendingPlay = true;
        audio->m_pendingStop = false;
    }
    if (auto* element = gameObject.GetComponent<VFXElement>()) {
        element->triggeredAt = -1.0f;
        element->lastLocalTime = -1.0f;
        element->progress = 0.0f;
        element->weight = 0.0f;
    }
}

/// @brief エンベロープが書き換えた値を捕獲時の値へ戻す。
/// @note captured を落とすと捕り直すたびに «書き換え後の値» を基準として掴んでしまい、
///       ループのたびに暗く (あるいは小さく) なっていく。
void RestoreEnvelopes(GameObject& gameObject)
{
    if (auto* env = gameObject.GetComponent<VFXLightEnvelope>()) {
        if (auto* light = gameObject.GetComponent<LightComponent>()) {
            VFXLightEnvelope::Base restored;
            if (env->base.Restore(restored)) {
                light->intensity = restored.intensity;
                light->color     = restored.color;
            }
        }
    }
    if (auto* env = gameObject.GetComponent<VFXTransformEnvelope>())
        env->base.Restore(gameObject.transform.scale);

    if (auto* env = gameObject.GetComponent<VFXDecalEnvelope>()) {
        if (auto* decal = gameObject.GetComponent<DecalComponent>())
            env->base.Restore(decal->emissiveScale);
    }
    if (auto* env = gameObject.GetComponent<VFXAudioEnvelope>()) {
        if (auto* audio = gameObject.GetComponent<AudioSourceComponent>()) {
            VFXAudioEnvelope::Base restored;
            if (env->base.Restore(restored)) {
                audio->volume = restored.volume;
                audio->pitch  = restored.pitch;
            }
        }
    }
    /// @note マテリアルは «元の値» ではなく «上書きが無い» 状態へ戻す。
    ///       自分が書いたキーだけを消す (手で足した override を巻き込まないため)。
    if (auto* env = gameObject.GetComponent<VFXMaterialEnvelope>()) {
        if (auto* material = gameObject.GetComponent<MaterialComponent>()) {
            if (!env->writtenColorParam.empty())
                material->paramOverrides.erase(env->writtenColorParam);
            if (!env->writtenParam.empty())
                material->paramOverrides.erase(env->writtenParam);
        }
        env->writtenColorParam.clear();
        env->writtenParam.clear();
    }
}

void SetElementActive(GameObject& gameObject, bool active)
{
    if (active) {
        gameObject.SetActive(true);
        RestoreEnvelopes(gameObject);
        RewindPlayback(gameObject);
        return;
    }

    if (auto* emitter = gameObject.GetComponent<ParticleEmitter>()) {
        emitter->settings.playing = false;
        emitter->runtime.particles.clear();
        emitter->runtime.gpuClearPending = true;
    }
    if (auto* audio = gameObject.GetComponent<AudioSourceComponent>()) {
        audio->m_pendingPlay = false;
        audio->m_pendingStop = true;
    }
    /// @note Scene::View は非アクティブ GameObject を除外しないため、画面演出は明示的に殺す。
    ///       これを怠ると窓が閉じた後もフラッシュ・揺れ・減速が残り続ける。
    if (auto* screen = gameObject.GetComponent<VFXScreenEffect>()) {
        screen->enabled = false;
        screen->weight = 0.0f;
        screen->progress = 0.0f;
    }
    if (auto* shake = gameObject.GetComponent<VFXCameraShake>()) {
        shake->enabled = false;
        shake->weight = 0.0f;
    }
    if (auto* scale = gameObject.GetComponent<VFXTimeScale>()) {
        scale->enabled = false;
        scale->weight = 0.0f;
    }
    RestoreEnvelopes(gameObject);
    gameObject.SetActive(false);
}

/// @brief 頭出しのついでに SubEmitter の探索範囲をこのルートへ閉じる。
/// @note SubEmitter の参照は GameObject の «名前» で、範囲が無いとシーン全体から引く
///       (ParticlePass::QueueSubEmitter)。同じ .vfx を 2 つ同時に鳴らすと片方の 1 段目が
///       もう片方の 2 段目を吹かせる ─ «たまに多い» という形で出るため量の設定ミスと
///       見分けが付かない。自分の枝を知るのはルートだけで、頭出しは枝を丸ごと歩く唯一の
///       場所。入れ子ルートはここで Restart() を積んで降りないので、内側は入れ子側の
///       ResetSubtree が自分を範囲として配り直す。
void ResetSubtree(GameObject& root)
{
    const EntityID scope = root.GetID();
    if (auto* emitter = root.GetComponent<ParticleEmitter>())
        emitter->settings.subEmitterScopeRoot = scope;

    ForEachDescendant(root, [scope](GameObject& child) {
        if (auto* nested = child.GetComponent<VFXComponent>()) {
            nested->Restart();
            return;
        }
        if (auto* emitter = child.GetComponent<ParticleEmitter>())
            emitter->settings.subEmitterScopeRoot = scope;
        RestoreEnvelopes(child);
        RewindPlayback(child);
    });
}

/// 進捗とエンベロープ

/// VFXElement を持たないオブジェクトの進捗 0..1。
/// §4.1 の「時間の正本は 1 オブジェクトにつき 1 つ」を、読む順序として書き下したもの。
[[nodiscard]] bool ResolveProgress(GameObject& gameObject, float rootTime, float rootSpan,
                                   float& outProgress)
{
    if (auto* decal = gameObject.GetComponent<DecalComponent>(); decal != nullptr && decal->lifetime > 0.0f) {
        outProgress = std::clamp(decal->age / decal->lifetime, 0.0f, 1.0f);
        return true;
    }
    if (auto* emitter = gameObject.GetComponent<ParticleEmitter>()) {
        const float span = (std::max)(emitter->settings.duration, kMinSpan);
        outProgress = std::clamp(emitter->runtime.playTime / span, 0.0f, 1.0f);
        return true;
    }
    if (rootSpan > 0.0f) {
        outProgress = std::clamp(rootTime / rootSpan, 0.0f, 1.0f);
        return true;
    }
    return false;
}

void ApplyEnvelopes(GameObject& gameObject, float progress)
{
    if (auto* env = gameObject.GetComponent<VFXLightEnvelope>(); env != nullptr && env->enabled) {
        if (auto* light = gameObject.GetComponent<LightComponent>()) {
            env->base.Capture({ light->intensity, light->color });
            if (env->useIntensityCurve) {
                light->intensity = env->base.value.intensity
                    * (std::max)(env->intensityCurve.Evaluate(progress), 0.0f);
            }
            if (env->useColorGradient) {
                /// @note alpha は明るさ倍率。色と強さを 1 本のグラデーションで作れるようにする。
                const math::Vector4 sampled = env->colorGradient.Evaluate(progress);
                light->color = { sampled.x * sampled.w, sampled.y * sampled.w, sampled.z * sampled.w };
            }
        }
    }

    if (auto* env = gameObject.GetComponent<VFXTransformEnvelope>();
        env != nullptr && env->enabled && env->useScaleCurve) {
        env->base.Capture(gameObject.transform.scale);
        gameObject.transform.scale = env->base.value * env->scaleCurve.Evaluate(progress);
    }

    if (auto* env = gameObject.GetComponent<VFXMaterialEnvelope>(); env != nullptr && env->enabled) {
        if (auto* material = gameObject.GetComponent<MaterialComponent>()) {
            /// @note 書いたキーを覚えておく。RestoreEnvelopes がこれだけを取り除く。
            if (env->useColorGradient && !env->colorParamName.empty()) {
                const math::Vector4 color = env->colorGradient.Evaluate(progress);
                material->paramOverrides[env->colorParamName] = { color.x, color.y, color.z, color.w };
                env->writtenColorParam = env->colorParamName;
            }
            if (!env->paramName.empty()) {
                material->paramOverrides[env->paramName] = { env->paramCurve.Evaluate(progress) };
                env->writtenParam = env->paramName;
            }
        }
    }

    if (auto* env = gameObject.GetComponent<VFXAudioEnvelope>(); env != nullptr && env->enabled) {
        if (auto* audio = gameObject.GetComponent<AudioSourceComponent>()) {
            env->base.Capture({ audio->volume, audio->pitch });
            audio->volume = env->base.value.volume
                * std::clamp(env->volumeCurve.Evaluate(progress), 0.0f, 1.0f);
            if (env->usePitchCurve) {
                audio->pitch = env->base.value.pitch
                    * (std::max)(env->pitchCurve.Evaluate(progress), 0.01f);
            }
        }
    }

    if (auto* env = gameObject.GetComponent<VFXDecalEnvelope>(); env != nullptr && env->enabled) {
        if (auto* decal = gameObject.GetComponent<DecalComponent>()) {
            env->base.Capture(decal->emissiveScale);
            /// @note albedoColor は組み込み経路の値で、.mat を割り当てた瞬間にシェーダーへ
            ///       届かなくなる。カーブで作った減り方がマテリアルの有無で消えては困るので、
            ///       opacity (投影側の倍率) へ掛ける。
            decal->opacity = std::clamp(env->fadeCurve.Evaluate(progress), 0.0f, 1.0f);
            /// @note エミッシブは組み込み経路のパラメータ。.mat 経路では材質が持つので触らない。
            if (env->driveEmissive && decal->materialPath.empty()) {
                decal->emissiveScale = env->base.value
                    * (std::max)(env->emissiveCurve.Evaluate(progress), 0.0f);
            }
        }
    }
}

/// @name 生存窓の評価

/// @brief 取り出さず «在るか» だけ見る。
/// @note 同じ trigger 名を待つ要素が複数あるのが普通で、最初の 1 つが消費すると残りが
///       永久に鳴らない。積まれた名前はフレーム末に一括で捨てる。
[[nodiscard]] bool HasTrigger(const std::vector<std::string>& pending, const std::string& name)
{
    return std::find(pending.begin(), pending.end(), name) != pending.end();
}

void ApplyElement(GameObject& gameObject, VFXElement& element, VFXComponent& vfx, float rootSpan)
{
    if (!element.enabled) return;

    float start = element.startDelay;
    if (!element.trigger.empty()) {
        if (element.triggeredAt < 0.0f) {
            if (!HasTrigger(vfx.pendingTriggers, element.trigger)) {
                if (gameObject.activeSelf()) SetElementActive(gameObject, false);
                element.progress = 0.0f;
                element.weight = 0.0f;
                return;
            }
            element.triggeredAt = vfx.time;
        }
        start = element.triggeredAt + element.startDelay;
    }

    /// @note duration が 0 以下なら「ルートが終わるまで」。
    float span = element.duration;
    if (span <= 0.0f) span = (std::max)(rootSpan - start, kMinSpan);

    float local = vfx.time - start;
    if (element.loop && local > 0.0f) {
        const float wrapped = std::fmod(local, span);
        /// @note 折り返しは active が切り替わらないため、時刻の巻き戻りで検出する。
        if (element.lastLocalTime >= 0.0f && wrapped < element.lastLocalTime)
            RewindPlayback(gameObject);
        local = wrapped;
    }
    element.lastLocalTime = local;

    const bool inside = local >= 0.0f && local < span;
    if (gameObject.activeSelf() != inside) SetElementActive(gameObject, inside);

    element.progress = std::clamp(local / span, 0.0f, 1.0f);
    element.weight = inside ? (std::max)(element.weightCurve.Evaluate(element.progress), 0.0f) : 0.0f;
    if (!inside) return;

    if (auto* screen = gameObject.GetComponent<VFXScreenEffect>()) {
        screen->weight = element.weight;
        /// @note 衝撃波リングの半径はこちらで走る
        screen->progress = element.progress;
    }
    if (auto* shake = gameObject.GetComponent<VFXCameraShake>()) {
        shake->weight = element.weight;
        /// @note 位相は窓内の経過秒。スクラブしても同じ揺れになる
        shake->elapsed = local;
    }
    if (auto* scale = gameObject.GetComponent<VFXTimeScale>()) scale->weight = element.weight;
}

void ApplySubtree(GameObject& root, VFXComponent& vfx)
{
    const float span = vfx.resolvedDuration;
    ForEachDescendant(root, [&](GameObject& child) {
        float progress = 0.0f;
        if (auto* element = child.GetComponent<VFXElement>()) {
            ApplyElement(child, *element, vfx, span);
            if (!child.activeSelf()) return;
            progress = element->progress;
        } else if (!ResolveProgress(child, vfx.time, span, progress)) {
            return;
        }
        ApplyEnvelopes(child, progress);
    });
}

/// @name ヒットストップの集計

/// @brief アクティブな VFXTimeScale を集計して Time::vfxTimeScale へ反映する。
/// @note 複数のヒットストップが重なるのは普通にあるため最も遅い要求を採用し、1 つも無い
///       状態では必ず 1.0 へ戻す (書き込みが残って世界が止まる事故を防ぐ)。専用の枠へ書く
///       のは、Time::timeScale がゲーム側 (スクリプト/Inspector) の枠で、こちらは
///       Phase::LateScript (スクリプトより後) で走るため。同じ変数へ書いていた頃は同じ
///       フレームのゲーム側ヒットストップ/スローを丸ごと踏み潰していたので、入力を 2 本に
///       割って Time::Tick が掛け合わせる形にした。
/// @param ownsTimeScale 呼び出し側が持つ「前フレームに自分が書いたか」の記憶。
void ApplyVFXTimeScale(Scene& scene, bool& ownsTimeScale)
{
    float slowest = 1.0f;
    bool any = false;
    for (auto [tf, request] : scene.View<Transform, VFXTimeScale>()) {
        if (!request.enabled) continue;
        const float weight = std::clamp(request.weight, 0.0f, 1.0f);
        if (weight <= 0.0f) continue;
        any = true;
        slowest = (std::min)(slowest, 1.0f + (request.timeScale - 1.0f) * weight);
    }
    /// @note VFX が書いた間だけ責任を持ち、要求が消えた最初のフレームで等速へ戻す。常時 1.0 を
    ///       書き戻すと、デバッグ用のスロー再生など外部の timeScale 設定を毎フレーム踏み潰す
    ///       ため、自分が触ったときだけ後始末する。
    if (any) {
        Time::vfxTimeScale = (std::max)(slowest, 0.0f);
        ownsTimeScale = true;
    } else if (ownsTimeScale) {
        Time::vfxTimeScale = 1.0f;
        ownsTimeScale = false;
    }
}

/// ルート 1 つぶんの更新

void FinishRoot(Scene& scene, GameObject& owner, VFXComponent& vfx)
{
    /// @note 次に起こされたときへ playOnAwake から初期化し直させる。
    vfx.initialized = false;
    vfx.playing = false;
    /// @note プレファブ由来なら «壊す» のではなくプールへ返す。次の 1 発が生成コストを払わずに済む。
    ///       Despawn は prefabAssetPath が空 (プレファブ由来でない) なら false を返すので、
    ///       «プールから来たか» を別に覚えておく必要は無い。
    if (PrefabPool::Despawn(scene, owner)) return;
    GameObject::Destroy(owner);
}

void UpdateRoot(SystemContext& ctx, GameObject& owner, VFXComponent& vfx, bool nested)
{
    /// @note 尺の算出は階層を丸ごと歩くので毎フレームは回さない。頭出しの瞬間と、
    ///       値が動きうるエディタ編集中だけ引き直す。
    if (!vfx.initialized || vfx.restartRequested || !ctx.simulating) {
        if (vfx.duration > 0.0f) {
            vfx.resolvedDuration = vfx.duration;
            vfx.endless = false;
        } else {
            vfx.resolvedDuration = ResolveDuration(owner, vfx.endless);
        }
    }

    if (!vfx.initialized) {
        vfx.initialized = true;
        vfx.time = 0.0f;
        /// @note playOnAwake は «誰も指示しなかったときの既定»、Restart() は «鳴らせ» という
        ///       明示の指示。初期化はこの実体を初めて見たフレームまで遅れるため、生成直後に
        ///       Restart() を呼ぶ経路 (プールの枠を鳴らす形) では指示より «後» にここが走り
        ///       既定に潰される。潰されると time が 0 で止まったまま «窓の中» と判定され続け、
        ///       光がひとつ点きっぱなしで残る。
        if (!vfx.restartRequested) vfx.playing = vfx.playOnAwake;
        ResetSubtree(owner);
    }
    if (vfx.restartRequested) {
        vfx.restartRequested = false;
        ResetSubtree(owner);
    }

    /// @note 書き込みが 2 フレーム以上途絶えたスクラブは «パネルが閉じた» とみなして手放す。
    const bool scrubbing = vfx.editorScrubTime >= 0.0f
        && vfx.editorScrubFrame + 2 >= Time::frameCount;
    if (scrubbing) vfx.time = vfx.editorScrubTime;
    else if (vfx.playing) vfx.time += ctx.dt * (std::max)(vfx.speed, 0.0f);

    ApplySubtree(owner, vfx);
    vfx.pendingTriggers.clear();

    const float span = vfx.resolvedDuration;
    if (vfx.endless || span <= 0.0f || scrubbing || vfx.time < span) return;

    if (vfx.loop) {
        vfx.time = std::fmod(vfx.time, span);
        ResetSubtree(owner);
        return;
    }
    if (!vfx.playing) return;
    vfx.playing = false;
    /// @note 入れ子ルートは親が畳むので自分では消えない。親より先に消えると
    ///       親のループが «子だけ居ない» 状態で回り始める。
    if (vfx.autoDestroy && !nested && ctx.simulating) FinishRoot(ctx.scene, owner, vfx);
}

} // namespace

ComponentAccess VFXSystem::GetAccess() const
{
    /// @note Scene 構造 (SetActive / Destroy) を変更するため、型単位の並列化対象から外す。
    return ComponentAccess{}.Unrestricted();
}

OrderingHints VFXSystem::GetOrder() const
{
    /// @note AudioSystem より先に音源を有効化する。Particle は後段 LateUpdate なので同フレームに評価される。
    return OrderingHints{}.Before<AudioSystem>().Before<AnimatorSystem>();
}

void VFXSystem::Update(SystemContext& ctx)
{
    /// @note ヒットストップはゲーム実行時だけ。エディタプレビューで効かせるとエディタ全体が遅くなる。
    if (ctx.simulating) ApplyVFXTimeScale(ctx.scene, m_ownsTimeScale);

    /// @note GetEntities は Scene 内部ストレージを指す span を返すため、ループ内で GameObject を
    ///       生成/破棄すると無効化される。イテレーション前にコピーしてスナップショットを取る。
    const std::span<const EntityID> view = ctx.scene.GetEntities<VFXComponent>();
    const std::vector<EntityID> roots(view.begin(), view.end());

    for (const EntityID id : roots) {
        GameObject* owner = ctx.scene.GetGameObject(id);
        VFXComponent* vfx = ctx.scene.GetComponent<VFXComponent>(id);
        if (owner == nullptr || vfx == nullptr) continue;
        if (!vfx->enabled) continue;
        if (!owner->activeInHierarchy()) {
            /// @note 非アクティブの間は時間を止め、次に起きたとき頭から鳴らす。
            vfx->initialized = false;
            continue;
        }
        const bool nested = HasAncestorVFX(*owner);
        vfx->nestingDepth = nested ? 1 : 0;
        UpdateRoot(ctx, *owner, *vfx, nested);
    }
}

GameObject* SpawnVFX(Scene& scene, const std::string& vfxPath,
                     math::Vector3 position, math::Quaternion rotation)
{
    if (vfxPath.empty()) return nullptr;
    GameObject* instance = PrefabPool::Spawn(scene, vfxPath, position, rotation);
    if (instance == nullptr) return nullptr;

    VFXComponent* vfx = instance->GetComponent<VFXComponent>();
    if (vfx == nullptr) {
        FBZZ_LOG_WARN("SpawnVFX: root has no VFXComponent -> %s", vfxPath.c_str());
        return instance;
    }
    /// @note playOnAwake ではなくここでの Restart を正とする
    vfx->initialized = true;
    vfx->Restart();
    return instance;
}

} // namespace fbzz::scene
