/// @file    RuntimeMeshSystem.hpp
/// @brief   実行中に決まるメッシュ (手続き生成 / meshPath 差し替え) を GPU へ載せる System
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// ProceduralMeshComponent のアップロードと MeshRenderer::meshPath の再解決を行う。
/// @note 2 つを 1 System にまとめる理由: どちらも «MeshRenderer::mesh の指す先を決め直す»
///       同じ仕事で ResourceManager を要る。ResourceManager が SystemContext に載るのは
///       Phase::LateUpdate だけなので、置ける場所も 1 箇所しかない。
class RuntimeMeshSystem final : public ISystem {
public:
    std::string_view Name() const override { return "RuntimeMeshSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    ComponentAccess GetAccess() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
