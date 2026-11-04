/// @file    RuntimeMeshSystem.hpp
/// @brief   実行中に決まるメッシュ (手続き生成 / meshPath 差し替え) を GPU へ載せる System
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// @note ProceduralMeshComponent のアップロードと MeshRenderer::meshPath の再解決を行う。
/// @note 2 つを 1 System にまとめる理由: どちらも «MeshRenderer::mesh の指す先を決め直す»
/// @note 同じ仕事で ResourceManager を要る。ResourceManager が SystemContext に載るのは
/// @note Phase::LateUpdate だけなので、置ける場所も 1 箇所しかない。
class RuntimeMeshSystem final : public ISystem {
public:
    std::string_view Name() const override { return "RuntimeMeshSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
