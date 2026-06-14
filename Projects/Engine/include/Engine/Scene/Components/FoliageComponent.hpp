// FBZZ Engine
// FoliageComponent.hpp | fbzz::scene
// Terrain 上へ樹木・大型岩などの複数 SubMesh 植生を配置する定義とランタイムキャッシュ
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

// FoliageInstance — Foliage.hlsl の StructuredBuffer と一致する GPU 転送データ。
struct FoliageInstance {
    float posX, posY, posZ;
    float rotY;
    float scale;
};
static_assert(sizeof(FoliageInstance) == 20, "FoliageInstance size mismatch");

enum class FoliagePlacementMode : uint8_t {
    PROCEDURAL,
    STAMP
};

// FoliageStamp — Scene に永続化する Terrain ローカル空間の手動配置。
// WHY: ワールド座標を保存すると Terrain の移動・回転後に植生だけが取り残されるため、
//      Terrain と同じローカル空間で位置を保持する。
struct FoliageStamp {
    math::Vector3 localPosition = {};
    float rotationY = 0.0f;
    float scale = 1.0f;
};

// FoliageSpecies — 1種類のモデルと、その SubMesh ごとの見た目・配置規則。
// WHY: 木は幹と葉で別 Material を必要とするため、DetailLayer の単一 Texture 参照から分離する。
struct FoliageSpecies {
    std::string modelPath;
    std::vector<std::string> subMeshMaterialPaths;
    FoliagePlacementMode placementMode = FoliagePlacementMode::PROCEDURAL;
    std::vector<FoliageStamp> stamps;
    float densityPer100SquareMeters = 0.5f;
    float minScale = 0.9f;
    float maxScale = 1.1f;
    float drawDistance = 150.0f;
    uint32_t seed = 1;
    bool randomYRotation = true;
};

// FoliageSpeciesCache — Species 1つ分の配置と GPU instance buffer。
struct FoliageSpeciesCache {
    std::vector<FoliageInstance> instances;
    renderer::ResourceHandle<renderer::StructuredBufferTag> instanceBuffer;
    std::vector<renderer::ResourceHandle<renderer::ConstantBufferTag>> materialConstantBuffers;
};

struct FoliageComponent {
    std::vector<FoliageSpecies> species;

    // 以下は Terrain/Transform から再生成できる非永続化キャッシュ。
    std::vector<FoliageSpeciesCache> caches;
    math::Vector3 bakedWorldPosition = {};
    math::Quaternion bakedWorldRotation = math::Quaternion::Identity();
    math::Vector3 bakedWorldScale = math::Vector3::ONE;
    bool hasBakedTransform = false;

    bool enabled = true;
    bool needsBake = true;

    const char* GetTypeName() const { return "Foliage"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        // species は vector<struct> のため SceneSerializer が直接扱う。
    }
};

} // namespace fbzz::scene
