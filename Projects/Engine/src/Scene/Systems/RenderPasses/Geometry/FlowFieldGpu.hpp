/// @file    FlowFieldGpu.hpp
/// @brief   ActiveFlowField を GPU の GpuFlowField へ詰める。GPU 粒子と表面繊維で共有する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Asset/VelocityFieldAtlas.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <bit>

namespace fbzz::scene {

/// @brief 1 本の流れを Rendering/FlowField.hlsli の SampleFlowFields が読む形へ詰める。
/// @note 焼いた場はアトラスへ常駐させてタイル番号で指す。常駐できなければ tile < 0 の «無効» として送り、本数をずらさない。
/// @note fieldTile.z は channels のビット列。GPU は asuint で読み、float として演算しない。
/// @see Docs/design/flow-field.md
[[nodiscard]] inline GpuFlowField PackGpuFlowField(const ActiveFlowField& field, renderer::ResourceManager& resources)
{
    GpuFlowField packed{};
    packed.posRadius = { field.position.x, field.position.y, field.position.z, field.radius };
    packed.dirStrength = { field.direction.x, field.direction.y, field.direction.z, field.strength };
    packed.params = { static_cast<float>(field.type), field.falloffPower, field.noiseFrequency, field.noiseSpeed };
    packed.fieldTile = { -1.0f, 1.0f, std::bit_cast<float>(field.channels), 0.0f };
    if (field.type == FlowFieldType::Baked && field.vectorField != nullptr) {
        const int tile = asset::VelocityFieldAtlas::Acquire(*field.vectorField, resources);
        packed.fieldRotation = { field.inverseRotation.x, field.inverseRotation.y,
                                 field.inverseRotation.z, field.inverseRotation.w };
        packed.fieldExtents = { field.extents.x, field.extents.y, field.extents.z, 0.0f };
        packed.fieldTile.x = static_cast<float>(tile);
        packed.fieldTile.y = field.vectorField->maxMagnitude;
    }
    return packed;
}

} // namespace fbzz::scene
