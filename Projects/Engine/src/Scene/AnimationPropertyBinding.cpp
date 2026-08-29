/// @file    AnimationPropertyBinding.cpp
/// @brief   propertyName から Reflect() の 1 フィールドを引き当てて読み書きする
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/AnimationPropertyBinding.hpp>

#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>

namespace fbzz::scene {
namespace {

// Reflect() を一度だけ巡回して、一致する永続キーへ値を書き込む。
class PropertyWriteReflector final : public IReflector {
public:
    PropertyWriteReflector(const asset::PropertyAnimationTrack& track, double ticks)
        : m_track(track), m_ticks(ticks) {}

    void Field(const char* name, float& value) override {
        if (Matches(name) && !m_track.floatKeys.empty())
            value = asset::SampleFloatKeys(m_track.floatKeys, m_ticks, m_track.interp);
    }
    void Field(const char* name, int& value) override {
        if (Matches(name) && !m_track.intKeys.empty())
            value = asset::SampleDiscreteKey(m_track.intKeys, m_ticks).value;
    }
    void Field(const char* name, bool& value) override {
        if (Matches(name) && !m_track.boolKeys.empty())
            value = asset::SampleDiscreteKey(m_track.boolKeys, m_ticks).value;
    }
    void Field(const char* name, math::Vector2& value) override {
        if (Matches(name) && !m_track.vector2Keys.empty())
            value = asset::SampleVector2Keys(m_track.vector2Keys, m_ticks, m_track.interp);
    }
    void Field(const char* name, math::Vector3& value) override {
        if (Matches(name) && !m_track.vector3Keys.empty())
            value = asset::SampleVectorKeys(m_track.vector3Keys, m_ticks, value, m_track.interp);
    }
    void Field(const char* name, math::Vector4& value) override {
        if (Matches(name) && !m_track.vector4Keys.empty())
            value = asset::SampleVector4Keys(m_track.vector4Keys, m_ticks, m_track.interp);
    }
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}

private:
    bool Matches(const char* fallback) const {
        return m_track.propertyName == PersistentKey(fallback);
    }
    const asset::PropertyAnimationTrack& m_track;
    double m_ticks = 0.0;
};

// 現在値を 1 件だけ拾う。
class PropertyCaptureReflector final : public IReflector {
public:
    PropertyCaptureReflector(const asset::PropertyAnimationTrack& track,
                             AnimationPropertySample& sample)
        : m_track(track), m_sample(sample) {}

    void Field(const char* name, float& value) override {
        if (!Matches(name)) return;
        m_sample.floats[0] = value;
        Mark(asset::AnimValueType::Float);
    }
    void Field(const char* name, int& value) override {
        if (!Matches(name)) return;
        m_sample.intValue = value;
        Mark(asset::AnimValueType::Int);
    }
    void Field(const char* name, bool& value) override {
        if (!Matches(name)) return;
        m_sample.boolValue = value;
        Mark(asset::AnimValueType::Bool);
    }
    void Field(const char* name, math::Vector2& value) override {
        if (!Matches(name)) return;
        m_sample.floats[0] = value.x;
        m_sample.floats[1] = value.y;
        Mark(asset::AnimValueType::Vector2);
    }
    void Field(const char* name, math::Vector3& value) override {
        if (!Matches(name)) return;
        m_sample.floats[0] = value.x;
        m_sample.floats[1] = value.y;
        m_sample.floats[2] = value.z;
        Mark(asset::AnimValueType::Vector3);
    }
    void Field(const char* name, math::Vector4& value) override {
        if (!Matches(name)) return;
        m_sample.floats[0] = value.x;
        m_sample.floats[1] = value.y;
        m_sample.floats[2] = value.z;
        m_sample.floats[3] = value.w;
        Mark(asset::AnimValueType::Vector4);
    }
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}

private:
    bool Matches(const char* fallback) const {
        return !m_sample.found && m_track.propertyName == PersistentKey(fallback);
    }
    void Mark(asset::AnimValueType type) {
        m_sample.type  = type;
        m_sample.found = true;
    }
    const asset::PropertyAnimationTrack& m_track;
    AnimationPropertySample& m_sample;
};

// スナップショットを書き戻す。型は捕まえたときのものを使う。
class PropertyRestoreReflector final : public IReflector {
public:
    PropertyRestoreReflector(const asset::PropertyAnimationTrack& track,
                             const AnimationPropertySample& sample)
        : m_track(track), m_sample(sample) {}

    void Field(const char* name, float& value) override {
        if (Matches(name, asset::AnimValueType::Float)) value = m_sample.floats[0];
    }
    void Field(const char* name, int& value) override {
        if (Matches(name, asset::AnimValueType::Int)) value = m_sample.intValue;
    }
    void Field(const char* name, bool& value) override {
        if (Matches(name, asset::AnimValueType::Bool)) value = m_sample.boolValue;
    }
    void Field(const char* name, math::Vector2& value) override {
        if (Matches(name, asset::AnimValueType::Vector2))
            value = { m_sample.floats[0], m_sample.floats[1] };
    }
    void Field(const char* name, math::Vector3& value) override {
        if (Matches(name, asset::AnimValueType::Vector3))
            value = { m_sample.floats[0], m_sample.floats[1], m_sample.floats[2] };
    }
    void Field(const char* name, math::Vector4& value) override {
        if (Matches(name, asset::AnimValueType::Vector4))
            value = { m_sample.floats[0], m_sample.floats[1],
                      m_sample.floats[2], m_sample.floats[3] };
    }
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}

private:
    bool Matches(const char* fallback, asset::AnimValueType type) const {
        return m_sample.found && m_sample.type == type
            && m_track.propertyName == PersistentKey(fallback);
    }
    const asset::PropertyAnimationTrack& m_track;
    const AnimationPropertySample& m_sample;
};

// componentType が指す実体 (登録コンポーネント → Script) へリフレクタを流す。
template<typename Reflector>
void VisitComponent(GameObject& target,
                    const asset::PropertyAnimationTrack& track,
                    Reflector&& make)
{
    bool visited = false;
    ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (visited || track.componentType != Registration::serializedName) return;
        if constexpr (requires(T& component, IReflector& reflector) {
            component.Reflect(reflector);
        }) {
            if (T* component = target.GetComponent<T>()) {
                make(*component);
                visited = true;
            }
        }
    });
    if (visited) return;

    if (ScriptComponent* scripts = target.GetComponent<ScriptComponent>()) {
        for (auto& entry : scripts->scripts) {
            if (!entry.script || track.componentType != entry.script->GetTypeName()) continue;
            make(*entry.script);
            break;
        }
    }
}

} // namespace

void ApplyComponentProperty(GameObject& target,
                            const asset::PropertyAnimationTrack& track,
                            double ticks)
{
    VisitComponent(target, track, [&](auto& reflectable) {
        PropertyWriteReflector reflector(track, ticks);
        reflectable.Reflect(reflector);
    });
}

void ApplyMaterialProperty(GameObject& target,
                           const asset::PropertyAnimationTrack& track,
                           double ticks)
{
    MaterialComponent* material = target.GetComponent<MaterialComponent>();
    if (!material || track.propertyName.empty()) return;
    // materialSlot が submesh (= マテリアルスロット) を選ぶ。負値は主スロット。
    const size_t slotIndex = track.materialSlot > 0 ? static_cast<size_t>(track.materialSlot) : 0u;
    auto& values = material->SlotAt(slotIndex).paramOverrides[track.propertyName];
    switch (track.valueType) {
    case asset::AnimValueType::Float:
        values = { asset::SampleFloatKeys(track.floatKeys, ticks, track.interp) };
        break;
    case asset::AnimValueType::Vector2: {
        const auto v = asset::SampleVector2Keys(track.vector2Keys, ticks, track.interp);
        values = { v.x, v.y };
        break;
    }
    case asset::AnimValueType::Vector3: {
        const auto v = asset::SampleVectorKeys(track.vector3Keys, ticks,
                                               math::Vector3::ZERO, track.interp);
        values = { v.x, v.y, v.z };
        break;
    }
    case asset::AnimValueType::Vector4:
    case asset::AnimValueType::Color: {
        const auto v = asset::SampleVector4Keys(track.vector4Keys, ticks, track.interp);
        values = { v.x, v.y, v.z, v.w };
        break;
    }
    case asset::AnimValueType::Int:
        if (!track.intKeys.empty())
            values = { static_cast<float>(asset::SampleDiscreteKey(track.intKeys, ticks).value) };
        break;
    case asset::AnimValueType::Bool:
        if (!track.boolKeys.empty())
            values = { asset::SampleDiscreteKey(track.boolKeys, ticks).value ? 1.0f : 0.0f };
        break;
    }
}

AnimationPropertySample CaptureComponentProperty(GameObject& target,
                                                 const asset::PropertyAnimationTrack& track)
{
    AnimationPropertySample sample;
    VisitComponent(target, track, [&](auto& reflectable) {
        PropertyCaptureReflector reflector(track, sample);
        reflectable.Reflect(reflector);
    });
    return sample;
}

void RestoreComponentProperty(GameObject& target,
                              const asset::PropertyAnimationTrack& track,
                              const AnimationPropertySample& sample)
{
    if (!sample.found) return;
    VisitComponent(target, track, [&](auto& reflectable) {
        PropertyRestoreReflector reflector(track, sample);
        reflectable.Reflect(reflector);
    });
}

bool CaptureMaterialProperty(GameObject& target,
                             const asset::PropertyAnimationTrack& track,
                             std::vector<float>& outValues)
{
    outValues.clear();
    MaterialComponent* material = target.GetComponent<MaterialComponent>();
    if (!material || track.propertyName.empty()) return false;
    const size_t slotIndex = track.materialSlot > 0 ? static_cast<size_t>(track.materialSlot) : 0u;
    if (slotIndex >= material->SlotCount()) return false;
    auto& overrides = material->SlotAt(slotIndex).paramOverrides;
    const auto it = overrides.find(track.propertyName);
    if (it == overrides.end()) return false;
    outValues = it->second;
    return true;
}

void RestoreMaterialProperty(GameObject& target,
                             const asset::PropertyAnimationTrack& track,
                             const std::vector<float>& values,
                             bool hadOverride)
{
    MaterialComponent* material = target.GetComponent<MaterialComponent>();
    if (!material || track.propertyName.empty()) return;
    const size_t slotIndex = track.materialSlot > 0 ? static_cast<size_t>(track.materialSlot) : 0u;
    if (slotIndex >= material->SlotCount()) return;
    auto& overrides = material->SlotAt(slotIndex).paramOverrides;
    // 演出が付けた override は、演出が終わったら跡形もなく消えるのが正しい。
    if (!hadOverride) overrides.erase(track.propertyName);
    else              overrides[track.propertyName] = values;
}

} // namespace fbzz::scene
