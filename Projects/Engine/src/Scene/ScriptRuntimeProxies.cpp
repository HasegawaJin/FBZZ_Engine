/// @file    ScriptRuntimeProxies.cpp
/// @brief   Runtime 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include "CoroutineFrameLoop.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Util/Easing.hpp>
#include <Engine/Util/Random.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Core/Logger.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {


/// @brief Editor ホスト中に game 側が要求したフルスクリーン状態。窓へは反映せず、
/// @note IsFullscreen() の答えだけ一致させる。
/// @note 覚えておく理由: 握り潰すだけだと Option 画面のチェックが押した直後に跳ね返り、
/// @note «Editor では設定 UI が壊れている» ように見える。ゲーム側コードは Standalone と
/// @note 1 行も変えずに済むのが望ましい。
/// @note 解像度側に同じ受け皿が無い理由: 読み戻す API の GetWidth()/GetHeight() は «実際に
/// @note 描いている面の寸法» であり、要求値を返すと嘘になるため。
bool s_editorRequestedFullscreen = false;


bool IsEditorHostedWindow()
{
    return core::Application::Get().IsEditorHosted();
}




/// @brief Tween 1 ステップぶんの経過時間。Scaled / Unscaled の分岐をここへ集約する。
float TweenStepDelta(const Script* script, TweenClock clock)
{
    if (!script) return 0.0f;
    return clock == TweenClock::Unscaled ? script->time.UnscaledDeltaTime()
                                         : script->time.DeltaTime();
}


} /// @note namespace
bool ScriptMemoryProxy::InitializeFrame(std::size_t capacity) const
{
    if (capacity == 0) {
        return false;
    }

    /// @note 既存領域を保持したまま容量だけ変えると旧ポインタの寿命が曖昧になるため、明示的に作り直す。
    if (m_frameAllocator.IsInitialized()) {
        m_frameAllocator.Shutdown();
    }

    return m_frameAllocator.Initialize(capacity);
}


void* ScriptMemoryProxy::AllocateFrame(std::size_t size, std::size_t alignment) const
{
    if (size == 0) {
        return nullptr;
    }

    /// @note 小規模 Script が OnAwake で明示初期化しなくても使えるよう、初回確保時に既定容量を用意する。
    if (!m_frameAllocator.IsInitialized() && !m_frameAllocator.Initialize(DEFAULT_FRAME_CAPACITY)) {
        return nullptr;
    }

    return m_frameAllocator.Allocate(size, alignment);
}


void ScriptMemoryProxy::ResetFrame() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return;
    }

    m_frameAllocator.Reset();
}


void ScriptMemoryProxy::ShutdownFrame() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return;
    }

    m_frameAllocator.Shutdown();
}


bool ScriptMemoryProxy::InitializePool(std::size_t blockSize,
                                       std::size_t blockCount,
                                       std::size_t alignment) const
{
    if (blockSize == 0 || blockCount == 0) {
        return false;
    }

    /// @note PoolAllocator は固定 stride のため、用途変更時は古いブロックを破棄してから作り直す。
    if (m_poolAllocator.IsInitialized()) {
        m_poolAllocator.Shutdown();
    }

    return m_poolAllocator.Initialize(blockSize, blockCount, alignment);
}


void* ScriptMemoryProxy::AllocatePool(std::size_t size, std::size_t alignment) const
{
    if (size == 0 || !m_poolAllocator.IsInitialized()) {
        return nullptr;
    }

    return m_poolAllocator.Allocate(size, alignment);
}


void ScriptMemoryProxy::FreePool(void* ptr) const
{
    if (ptr == nullptr || !m_poolAllocator.IsInitialized()) {
        return;
    }

    m_poolAllocator.Free(ptr);
}


void ScriptMemoryProxy::ResetPool() const
{
    if (!m_poolAllocator.IsInitialized()) {
        return;
    }

    m_poolAllocator.Reset();
}


void ScriptMemoryProxy::ShutdownPool() const
{
    if (!m_poolAllocator.IsInitialized()) {
        return;
    }

    m_poolAllocator.Shutdown();
}


bool ScriptMemoryProxy::IsFrameInitialized() const
{
    return m_frameAllocator.IsInitialized();
}


bool ScriptMemoryProxy::IsPoolInitialized() const
{
    return m_poolAllocator.IsInitialized();
}


core::MemoryStats ScriptMemoryProxy::GetFrameStats() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return {};
    }

    return m_frameAllocator.GetStats();
}


core::MemoryStats ScriptMemoryProxy::GetPoolStats() const
{
    if (!m_poolAllocator.IsInitialized()) {
        return {};
    }

    return m_poolAllocator.GetStats();
}


void ScriptMemoryProxy::BeginFrame() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return;
    }

    m_frameAllocator.BeginFrame();
}


Transform* ScriptTransformProxy::Get() const
{
    return script && script->m_gameObject ? &script->m_gameObject->transform : nullptr;
}

/// @name ローカル空間 property getter / setter

math::Vector3 ScriptTransformProxy::_GetPos() const
{
    auto* t = Get(); return t ? t->position : math::Vector3::ZERO;
}

void ScriptTransformProxy::_SetPos(const math::Vector3& v)
{
    if (auto* t = Get()) {
        t->position = v;
        FlushWorldTransforms(*script->m_gameObject);
    }
}

math::Quaternion ScriptTransformProxy::_GetRot() const
{
    auto* t = Get(); return t ? t->rotation : math::Quaternion::Identity();
}

void ScriptTransformProxy::_SetRot(const math::Quaternion& v)
{
    if (auto* t = Get()) {
        t->rotation = v;
        FlushWorldTransforms(*script->m_gameObject);
    }
}

math::Vector3 ScriptTransformProxy::_GetScl() const
{
    auto* t = Get(); return t ? t->scale : math::Vector3::ONE;
}

void ScriptTransformProxy::_SetScl(const math::Vector3& v)
{
    if (auto* t = Get()) {
        t->scale = v;
        FlushWorldTransforms(*script->m_gameObject);
    }
}

/// @name ワールド空間 property getter / setter

math::Vector3 ScriptTransformProxy::_GetWPos() const
{
    auto* t = Get(); return t ? t->worldPosition : math::Vector3::ZERO;
}

void ScriptTransformProxy::_SetWPos(const math::Vector3& v)
{
    if (auto* t = Get()) {
        GameObject& go = *script->m_gameObject;
        const math::Vector3 worldPosition = v;
        FlushWorldTransforms(go);
        const math::Quaternion localRotation = t->rotation;
        const math::Vector3 localScale = t->scale;
        SetWorldPose(go, worldPosition, t->worldRotation, t->worldScale);
        /// @note 位置だけの編集で、非可逆な親の軸にあるローカルスケールを失わない。
        t->rotation = localRotation;
        t->scale = localScale;
        FlushWorldTransforms(go);
    }
}

math::Quaternion ScriptTransformProxy::_GetWRot() const
{
    auto* t = Get(); return t ? t->worldRotation : math::Quaternion::Identity();
}

/// @name 算出値

math::Vector3 ScriptTransformProxy::_GetFwd()   const { auto* t = Get(); return t ? t->Forward() : math::Vector3::FORWARD; }

math::Vector3 ScriptTransformProxy::_GetUp()    const { auto* t = Get(); return t ? t->Up()      : math::Vector3::UP; }

math::Vector3 ScriptTransformProxy::_GetRight() const { auto* t = Get(); return t ? t->Right()   : math::Vector3::RIGHT; }

/// @name メソッド

void ScriptTransformProxy::Translate(const math::Vector3& v) const
{
    if (auto* t = Get()) {
        t->Translate(v);
        FlushWorldTransforms(*script->m_gameObject);
    }
}


void ScriptTransformProxy::Rotate(const math::Vector3& axis, float deg) const
{
    if (auto* t = Get()) {
        /// @note 長さゼロの軸は正規化の契約を満たさないため無視する。
        if (axis.LengthSq() < math::EPSILON * math::EPSILON) return;
        const auto delta = math::Quaternion::FromAxisAngle(axis.Normalized(), deg * DEG_TO_RAD);
        t->rotation = (delta * t->rotation).Normalized();
        FlushWorldTransforms(*script->m_gameObject);
    }
}


void ScriptTransformProxy::LookAt(const math::Vector3& target) const
{
    if (auto* t = Get()) {
        const math::Vector3 worldTarget = target;
        FlushWorldTransforms(*script->m_gameObject);
        t->LookAt(worldTarget);
        FlushWorldTransforms(*script->m_gameObject);
    }
}


float ScriptTransformProxy::DistanceTo(const GameObject& other) const
{
    const auto* t = Get();
    return t ? (other.transform.worldPosition - t->worldPosition).Length() : 0.0f;
}


math::Vector3 ScriptTransformProxy::DirectionTo(const GameObject& other) const
{
    const auto* t = Get();
    /// @note 相手と重なっている間は向きが無い。距離 0 は追跡や射撃で普通に通る状態なので、
    /// @note 「方向なし」を表す ZERO をそのまま返す (呼び出し側は既に 0 判定を持っている)。
    return t ? (other.transform.worldPosition - t->worldPosition).NormalizedOr(math::Vector3::ZERO)
             : math::Vector3::ZERO;
}


CursorRequest ScriptCursorProxy::Push(CursorLockMode mode, bool visible, int priority) const
{
    /// @note owner に index+1 を渡す。index 0 は正当な GameObject なので、
    /// @note 0 を «無所属» に使っている Cursor 側と衝突させない。
    std::uint32_t owner = 0;
    if (script) {
        if (GameObject* self = script->scene.Self(); self && self->IsValid())
            owner = self->GetID().index + 1;
    }
    const char* label = script ? script->GetTypeName() : nullptr;
    return CursorRequest(core::Cursor::Push({ mode, visible }, priority, owner, label));
}


void ScriptCursorProxy::SetVisible(bool visible) const
{
    core::Cursor::SetVisible(visible);
}


bool ScriptCursorProxy::IsVisible() const
{
    return core::Cursor::IsVisible();
}


void ScriptCursorProxy::SetLockMode(CursorLockMode mode) const
{
    core::Cursor::SetLockMode(mode);
}


CursorLockMode ScriptCursorProxy::GetLockMode() const
{
    return core::Cursor::GetLockMode();
}


void ScriptCursorProxy::SetShape(CursorShape shape) const
{
    core::Cursor::SetShape(shape);
}


CursorShape ScriptCursorProxy::GetShape() const
{
    return core::Cursor::GetShape();
}


bool ScriptCursorProxy::HasShapeImage(CursorShape shape) const
{
    return core::Cursor::HasShapeImage(shape);
}


void ScriptCursorProxy::ResetForEditor() const
{
    core::Cursor::ResetForEditor();
}


void ScriptApplicationProxy::Quit() const
{
    const GameObject* self = script ? script->scene.Self() : nullptr;
    if (!self || !self->GetScene() || !ScriptRuntime::RequestQuit(self->GetScene()->GetRenderSceneGeneration()))
        FBZZ_LOG_WARN("Application.Quit requires a live Play Scene.");
}


bool ScriptApplicationProxy::IsRunning() const
{
    return core::Application::Get().IsRunning();
}


uint32_t ScriptApplicationProxy::GetWindowWidth() const
{
    return core::Application::Get().GetWindowWidth();
}


uint32_t ScriptApplicationProxy::GetWindowHeight() const
{
    return core::Application::Get().GetWindowHeight();
}


bool ScriptApplicationProxy::IsPlaying() const
{
    return Script::IsInPlayMode();
}


bool ScriptApplicationProxy::IsEditMode() const
{
    return !Script::IsInPlayMode();
}


float ScriptTimeProxy::DeltaTime() const { return fbzz::Time::deltaTime; }

float ScriptTimeProxy::UnscaledDeltaTime() const { return fbzz::Time::unscaledDeltaTime; }

float ScriptTimeProxy::FixedDeltaTime() const { return fbzz::Time::fixedDeltaTime; }

float ScriptTimeProxy::Time() const { return fbzz::Time::time; }

float ScriptTimeProxy::UnscaledTime() const { return fbzz::Time::unscaledTime; }

uint64_t ScriptTimeProxy::FrameCount() const { return fbzz::Time::frameCount; }


void ScriptTimeProxy::SetTimeScale(float scale) const { fbzz::Time::SetTimeScale(scale); }

float ScriptTimeProxy::GetTimeScale() const { return fbzz::Time::GetTimeScale(); }

void ScriptTimeProxy::SetTargetFps(int fps) const { fbzz::Time::SetTargetFps(fps); }

int ScriptTimeProxy::GetTargetFps() const { return fbzz::Time::GetTargetFps(); }


void ScriptDisplayProxy::SetFullscreen(bool enabled) const
{
    if (IsEditorHostedWindow()) {
        s_editorRequestedFullscreen = enabled;
        return;
    }
    core::Application::Get().GetWindow().SetWindowMode(
        enabled ? core::WindowMode::BorderlessFullscreen : core::WindowMode::Windowed);
}


bool ScriptDisplayProxy::IsFullscreen() const
{
    if (IsEditorHostedWindow()) return s_editorRequestedFullscreen;
    return core::Application::Get().GetWindow().GetWindowMode()
        == core::WindowMode::BorderlessFullscreen;
}


void ScriptDisplayProxy::SetResolution(uint32_t width, uint32_t height) const
{
    if (IsEditorHostedWindow()) return;
    core::Application::Get().GetWindow().SetClientSize(width, height);
}


uint32_t ScriptDisplayProxy::GetWidth() const
{
    return core::Application::Get().GetWindowWidth();
}


uint32_t ScriptDisplayProxy::GetHeight() const
{
    return core::Application::Get().GetWindowHeight();
}


DisplayResolution ScriptDisplayProxy::GetMonitorSize() const
{
    DisplayResolution size;
    core::Application::Get().GetWindow().GetMonitorSize(size.width, size.height);
    return size;
}


std::vector<DisplayResolution> ScriptDisplayProxy::EnumResolutions() const
{
    const auto modes = core::Application::Get().GetWindow().EnumerateResolutions();
    std::vector<DisplayResolution> out;
    out.reserve(modes.size());
    for (const auto& [width, height] : modes) out.push_back(DisplayResolution{ width, height });
    return out;
}


void ScriptDisplayProxy::SetVSync(bool enabled) const
{
    core::Application::Get().GetRenderer().SetVSync(enabled);
}


bool ScriptDisplayProxy::GetVSync() const
{
    return core::Application::Get().GetRenderer().GetVSync();
}

/// @name ScriptGraphicsProxy
/// @brief 触る先は「実行中の RenderSettings」。Editor では Play 中しか登録されないので、
/// @brief 編集中に呼ばれた場合は静かに何もしない (ProjectSettings.toml を汚さないため)。


void ScriptEventProxy::UnsubscribeAll() const
{
    ScriptEventBus::UnsubscribeOwner(script);
}

/// @name ScriptRandomProxy


float ScriptRandomProxy::Value() const
{
    return util::Random::Value();
}


float ScriptRandomProxy::Range(float min, float max) const
{
    return util::Random::Range(min, max);
}


int ScriptRandomProxy::Range(int min, int max) const
{
    return util::Random::Range(min, max);
}


bool ScriptRandomProxy::Chance(float probability) const
{
    return util::Random::Value() < probability;
}


float ScriptRandomProxy::Sign() const
{
    return util::Random::Value() < 0.5f ? -1.0f : 1.0f;
}


math::Vector2 ScriptRandomProxy::InsideUnitCircle() const
{
    return util::Random::InsideUnitCircle();
}


math::Vector2 ScriptRandomProxy::OnUnitCircle() const
{
    return util::Random::OnUnitCircle();
}


math::Vector3 ScriptRandomProxy::InsideUnitSphere() const
{
    return util::Random::InsideUnitSphere();
}


math::Vector3 ScriptRandomProxy::OnUnitSphere() const
{
    return util::Random::OnUnitSphere();
}


math::Vector3 ScriptRandomProxy::ConeDirection(const math::Vector3& axis, float maxAngleDeg) const
{
    const math::Vector3 forward = axis.NormalizedOr(math::Vector3::FORWARD);
    if (maxAngleDeg <= 0.0f) return forward;

    /// @note 円錐内の一様サンプリング。cos を一様に引くことで、頂点付近に偏らせない。
    const float maxCos = std::cos(std::clamp(maxAngleDeg, 0.0f, 180.0f) * DEG_TO_RAD);
    const float cosTheta = util::Random::Range(maxCos, 1.0f);
    const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = util::Random::Range(0.0f, 6.28318530718f);

    /// @note forward に直交する基底を作る。forward と平行になりにくい軸を選んで外積する。
    const math::Vector3 reference =
        std::abs(forward.y) < 0.99f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
                                    : math::Vector3{ 1.0f, 0.0f, 0.0f };
    const math::Vector3 right = math::Vector3::Cross(forward, reference).Normalized();
    const math::Vector3 up    = math::Vector3::Cross(right, forward);

    return (right * (sinTheta * std::cos(phi))
          + up    * (sinTheta * std::sin(phi))
          + forward * cosTheta).Normalized();
}


void ScriptRandomProxy::SetSeed(uint32_t seed) const
{
    util::Random::SetSeed(static_cast<unsigned int>(seed));
}

/// @name ScriptTweenProxy


float ScriptTweenProxy::Evaluate(TweenEase ease, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (ease) {
    case TweenEase::InQuad:     return util::Easing::EaseInQuad(t);
    case TweenEase::OutQuad:    return util::Easing::EaseOutQuad(t);
    case TweenEase::InOutQuad:  return util::Easing::EaseInOutQuad(t);
    case TweenEase::InCubic:    return util::Easing::EaseInCubic(t);
    case TweenEase::OutCubic:   return util::Easing::EaseOutCubic(t);
    case TweenEase::InOutCubic: return util::Easing::EaseInOutCubic(t);
    case TweenEase::InSine:     return util::Easing::EaseInSine(t);
    case TweenEase::OutSine:    return util::Easing::EaseOutSine(t);
    case TweenEase::InOutSine:  return util::Easing::EaseInOutSine(t);
    case TweenEase::InExpo:     return util::Easing::EaseInExpo(t);
    case TweenEase::OutExpo:    return util::Easing::EaseOutExpo(t);
    case TweenEase::InOutExpo:  return util::Easing::EaseInOutExpo(t);
    case TweenEase::InBack:     return util::Easing::EaseInBack(t);
    case TweenEase::OutBack:    return util::Easing::EaseOutBack(t);
    case TweenEase::InOutBack:  return util::Easing::EaseInOutBack(t);
    case TweenEase::OutElastic: return util::Easing::EaseOutElastic(t);
    case TweenEase::OutBounce:  return util::Easing::EaseOutBounce(t);
    case TweenEase::Linear:
    default:                    return t;
    }
}


Coroutine ScriptTweenProxy::MoveTo(math::Vector3 target, float duration,
    TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) return {};
    if (std::isnan(duration) || duration <= 0.0f) {
        owner->transform.position = target;
        return {};
    }
    const math::Vector3 start = owner->transform.position;
    return coroutine_detail::RunFrames([owner, start, target, duration, ease, clock, elapsed = 0.0f]() mutable {
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        owner->transform.position = math::Vector3::Lerp(start, target, t);
        if (elapsed < duration) return true;
        owner->transform.position = target;
        return false;
    });
}

Coroutine ScriptTweenProxy::MoveBy(math::Vector3 delta, float duration,
    TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) return {};
    const math::Vector3 start = owner->transform.position;
    const math::Vector3 target = start + delta;
    if (std::isnan(duration) || duration <= 0.0f) {
        owner->transform.position = target;
        return {};
    }
    return coroutine_detail::RunFrames([owner, start, target, duration, ease, clock, elapsed = 0.0f]() mutable {
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        owner->transform.position = math::Vector3::Lerp(start, target, t);
        if (elapsed < duration) return true;
        owner->transform.position = target;
        return false;
    });
}

Coroutine ScriptTweenProxy::ScaleTo(math::Vector3 target, float duration,
    TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) return {};
    if (std::isnan(duration) || duration <= 0.0f) {
        owner->transform.scale = target;
        return {};
    }
    const math::Vector3 start = owner->transform.scale;
    return coroutine_detail::RunFrames([owner, start, target, duration, ease, clock, elapsed = 0.0f]() mutable {
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        owner->transform.scale = math::Vector3::Lerp(start, target, t);
        if (elapsed < duration) return true;
        owner->transform.scale = target;
        return false;
    });
}

Coroutine ScriptTweenProxy::RotateTo(math::Quaternion target, float duration,
    TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) return {};
    if (std::isnan(duration) || duration <= 0.0f) {
        owner->transform.rotation = target;
        return {};
    }
    const math::Quaternion start = owner->transform.rotation;
    return coroutine_detail::RunFrames([owner, start, target, duration, ease, clock, elapsed = 0.0f]() mutable {
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        owner->transform.rotation = math::Quaternion::Slerp(start, target, t);
        if (elapsed < duration) return true;
        owner->transform.rotation = target;
        return false;
    });
}

Coroutine ScriptTweenProxy::Value(float from, float to, float duration,
    std::function<void(float)> apply, TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner || !apply) return {};
    if (std::isnan(duration) || duration <= 0.0f) {
        apply(to);
        return {};
    }
    return coroutine_detail::RunFrames([owner, from, to, duration, apply = std::move(apply),
        ease, clock, elapsed = 0.0f]() mutable {
        elapsed += TweenStepDelta(owner, clock);
        const bool complete = elapsed >= duration;
        const float value = complete ? to : from + (to - from) * Evaluate(ease, elapsed / duration);
        apply(value);
        return !complete;
    });
}

Coroutine ScriptTweenProxy::ShakePosition(float amplitude, float duration,
    float frequency, TweenClock clock) const
{
    Script* owner = script;
    if (!owner || std::isnan(duration) || duration <= 0.0f || amplitude <= 0.0f) return {};
    const math::Vector3 origin = owner->transform.position;
    /// @note 固定した二軸を正弦で進め、フレームレートによらず同じ位相と減衰を使う。
    util::RandomStream rng{static_cast<uint64_t>(static_cast<uint32_t>(amplitude * 1000.0f) + 1u)};
    const math::Vector3 axisA = rng.OnUnitSphere();
    const math::Vector3 axisB = rng.OnUnitSphere();
    return coroutine_detail::RunFrames([owner, origin, axisA, axisB, amplitude, duration,
        frequency, clock, elapsed = 0.0f]() mutable {
        elapsed += TweenStepDelta(owner, clock);
        const float normalized = std::clamp(elapsed / duration, 0.0f, 1.0f);
        const float decay = 1.0f - normalized;
        const float phase = elapsed * frequency;
        const math::Vector3 offset = axisA * (std::sin(phase) * amplitude * decay)
            + axisB * (std::cos(phase * 1.37f) * amplitude * decay);
        owner->transform.position = origin + offset;
        if (elapsed < duration) return true;
        owner->transform.position = origin;
        return false;
    });
}

/// @name ScriptSequenceProxy


} /// @note namespace fbzz::scene
