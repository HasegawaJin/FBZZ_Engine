// FBZZ Engine
// VFXParameterRuntime.hpp | fbzz::asset
// VFX公開パラメーターのoverride選択、決定論値評価、schema binding適用
#pragma once

#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace fbzz::asset {

[[nodiscard]] inline const VFXParamDefinition* FindVFXParameter(
    const VFXGraphAsset& graph, std::string_view name)
{
    const auto iterator = std::find_if(graph.parameters.begin(), graph.parameters.end(),
        [name](const VFXParamDefinition& parameter) { return parameter.name == name; });
    return iterator == graph.parameters.end() ? nullptr : &*iterator;
}

[[nodiscard]] inline const VFXParamValue* ResolveVFXParamSource(
    const VFXGraphAsset& graph, const scene::VFXGraphComponent& component,
    const VFXParamDefinition& definition)
{
    const auto instance = std::find_if(component.parameterOverrides.begin(), component.parameterOverrides.end(),
        [&](const VFXParamOverride& value) { return value.paramName == definition.name; });
    if (instance != component.parameterOverrides.end()) return &instance->value;
    if (!component.variant.empty()) {
        const auto variant = std::find_if(graph.variants.begin(), graph.variants.end(),
            [&](const VFXVariantSet& value) { return value.name == component.variant; });
        if (variant != graph.variants.end()) {
            const auto overrideValue = std::find_if(variant->overrides.begin(), variant->overrides.end(),
                [&](const VFXParamOverride& value) { return value.paramName == definition.name; });
            if (overrideValue != variant->overrides.end()) return &overrideValue->value;
        }
    }
    return &definition.defaultValue;
}

[[nodiscard]] inline float DeterministicVFXRandom(std::uint32_t seed, std::string_view name)
{
    std::uint32_t hash = seed != 0 ? seed : 1u;
    for (const char character : name) hash = (hash ^ static_cast<std::uint8_t>(character)) * 16777619u;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    return static_cast<float>(hash & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

[[nodiscard]] inline float EvaluateVFXSignalNode(
    const VFXGraphAsset& graph, int nodeId, float time,
    std::unordered_map<int, float>& cache, int depth = 0)
{
    if (depth > 32) return 0.0f;
    if (const auto found = cache.find(nodeId); found != cache.end()) return found->second;
    const auto iterator = std::find_if(graph.signalNodes.begin(), graph.signalNodes.end(),
        [nodeId](const VFXSignalNode& node) { return node.id == nodeId; });
    if (iterator == graph.signalNodes.end()) return 0.0f;
    const auto input = [&](int id) { return id >= 0
        ? EvaluateVFXSignalNode(graph, id, time, cache, depth + 1) : 0.0f; };
    const float a = iterator->inputA >= 0 ? input(iterator->inputA) : iterator->valueA;
    const float b = iterator->inputB >= 0 ? input(iterator->inputB) : iterator->valueB;
    float result = 0.0f;
    switch (iterator->operation) {
    case VFXSignalOperation::Constant: result = iterator->valueA; break;
    case VFXSignalOperation::Time: result = time; break;
    case VFXSignalOperation::Sine: result = std::sin(a); break;
    case VFXSignalOperation::Noise: result = DeterministicVFXRandom(
        static_cast<std::uint32_t>(std::fabs(a) * 10000.0f) + 1u, "signal"); break;
    case VFXSignalOperation::Add: result = a + b; break;
    case VFXSignalOperation::Subtract: result = a - b; break;
    case VFXSignalOperation::Multiply: result = a * b; break;
    case VFXSignalOperation::Divide: result = std::fabs(b) > 0.000001f ? a / b : 0.0f; break;
    case VFXSignalOperation::Remap: result = iterator->valueA + a * (iterator->valueB - iterator->valueA); break;
    }
    cache[nodeId] = result;
    return result;
}

[[nodiscard]] inline bool EvaluateVFXParamValue(
    const VFXGraphAsset& graph, const VFXParamValue& value,
    const reflection::PropertyDesc& target, float normalizedTime,
    std::uint32_t seed, std::string_view parameterName, std::any& output)
{
    if (const auto* constant = std::get_if<VFXConstant>(&value.source)) {
        if (const auto* item = std::get_if<float>(constant)) output = *item;
        else if (const auto* item = std::get_if<int>(constant)) output = *item;
        else if (const auto* item = std::get_if<bool>(constant)) output = *item;
        else if (const auto* item = std::get_if<math::Vector4>(constant)) {
            output = target.type == reflection::PropertyType::Vector3
                ? std::any{ math::Vector3{ item->x, item->y, item->z } } : std::any{ *item };
        } else if (const auto* item = std::get_if<math::Vector3>(constant)) output = *item;
        else if (const auto* item = std::get_if<std::string>(constant)) output = *item;
        return output.has_value();
    }
    if (const auto* curve = std::get_if<VFXCurveSource>(&value.source)) {
        output = target.type == reflection::PropertyType::Curve
            ? std::any{ curve->curve } : std::any{ curve->curve.Evaluate(normalizedTime) };
        return true;
    }
    if (const auto* gradient = std::get_if<VFXGradientSource>(&value.source)) {
        output = target.type == reflection::PropertyType::Gradient
            ? std::any{ gradient->gradient } : std::any{ gradient->gradient.Evaluate(normalizedTime) };
        return true;
    }
    if (const auto* random = std::get_if<VFXRandomRange>(&value.source)) {
        const float alpha = DeterministicVFXRandom(seed, parameterName);
        output = random->minimum + (random->maximum - random->minimum) * alpha;
        return true;
    }
    if (const auto* signal = std::get_if<VFXSignalRef>(&value.source)) {
        const auto outputIterator = std::find_if(graph.signalOutputs.begin(), graph.signalOutputs.end(),
            [&](const VFXSignalOutput& item) { return item.name == signal->signalName; });
        if (outputIterator == graph.signalOutputs.end()) return false;
        std::unordered_map<int, float> cache;
        output = EvaluateVFXSignalNode(graph, outputIterator->nodeId, normalizedTime, cache);
        return true;
    }
    // AttributeRefはVFXGraphSystemの動的属性resolverが値を供給する。未接続時はdefaultへフォールバックする。
    return false;
}

inline void ApplyVFXBindings(const VFXGraphAsset& graph,
                             const scene::VFXGraphComponent& component,
                             VFXGraphNode& node, float normalizedTime)
{
    for (const VFXParamBinding& binding : graph.bindings) {
        if (binding.nodeId != node.id) continue;
        const VFXParamDefinition* definition = FindVFXParameter(graph, binding.paramName);
        if (definition == nullptr) continue;
        reflection::ResolvedProperty resolved;
        if (!reflection::ResolveProperty(GetVFXNodeSchema(), &node, binding.schemaPath, resolved)
            || resolved.property == nullptr || resolved.property->set == nullptr) continue;
        const VFXParamValue* source = ResolveVFXParamSource(graph, component, *definition);
        std::any value;
        if (source != nullptr && EvaluateVFXParamValue(
                graph, *source, *resolved.property, normalizedTime,
                node.particle.randomSeed, definition->name, value))
            resolved.property->set(resolved.owner, value);
    }
}

} // namespace fbzz::asset
