// FBZZ Engine
// VFXParameter.hpp | fbzz::asset
// VFX公開パラメーター、値ソース、binding、variant、signalの共有データ定義
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace fbzz::asset {

enum class VFXParamType : std::uint8_t { Float, Int, Bool, Color, Vector3, AssetRef };

using VFXConstant = std::variant<float, int, bool, math::Vector4, math::Vector3, std::string>;

struct VFXCurveSource { scene::ParticleCurve curve; };
struct VFXGradientSource { scene::ParticleGradient gradient; };
struct VFXRandomRange { float minimum = 0.0f; float maximum = 1.0f; };
struct VFXAttributeRef { std::string path; };
struct VFXSignalRef { std::string signalName; };

// default、override、動的sourceをこのvariantへ集約し、解決処理の分岐を一箇所に保つ。
struct VFXParamValue {
    std::variant<VFXConstant, VFXCurveSource, VFXGradientSource,
                 VFXRandomRange, VFXAttributeRef, VFXSignalRef> source = VFXConstant{ 0.0f };
};

struct VFXParamDefinition {
    std::string name;
    VFXParamType type = VFXParamType::Float;
    VFXParamValue defaultValue;
    float minimum = 0.0f;
    float maximum = 1.0f;
    bool hasRange = false;
};

struct VFXParamBinding {
    std::string paramName;
    int nodeId = 0;
    std::string schemaPath;
};

struct VFXParamOverride {
    std::string paramName;
    VFXParamValue value;
};

struct VFXVariantSet {
    std::string name;
    std::vector<VFXParamOverride> overrides;
};

struct VFXSubGraphForward {
    int nodeId = 0;
    std::string parentParam;
    std::string childParam;
};

enum class VFXSignalOperation : std::uint8_t {
    Constant,
    Time,
    Sine,
    Noise,
    Add,
    Subtract,
    Multiply,
    Divide,
    Remap,
};

struct VFXSignalNode {
    int id = 0;
    VFXSignalOperation operation = VFXSignalOperation::Constant;
    int inputA = -1;
    int inputB = -1;
    float valueA = 0.0f;
    float valueB = 1.0f;
};

struct VFXSignalOutput {
    std::string name;
    int nodeId = 0;
};

// Sceneの既存IReflector ABIを変更せず、複合overrideを単一string fieldとして往復させるcodec。
[[nodiscard]] std::string SerializeVFXOverrides(const std::vector<VFXParamOverride>& overrides);
[[nodiscard]] bool DeserializeVFXOverrides(const std::string& text,
                                           std::vector<VFXParamOverride>& overrides);

} // namespace fbzz::asset
