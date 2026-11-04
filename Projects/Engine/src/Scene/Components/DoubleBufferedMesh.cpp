/// @file    DoubleBufferedMesh.cpp
/// @brief   個体専用の二枚メッシュを単独所有し、コピーへ GPU 資源を持ち越さない。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Graphics/Renderer/OwnedResource.hpp>
#include <utility>

namespace fbzz::scene {
DoubleBufferedMesh::~DoubleBufferedMesh() { Reset(); }
DoubleBufferedMesh::DoubleBufferedMesh(DoubleBufferedMesh&& other) noexcept { *this = std::move(other); }
DoubleBufferedMesh& DoubleBufferedMesh::operator=(DoubleBufferedMesh&& other) noexcept
{
    if (this == &other) return *this;
    Reset();
    for (size_t index = 0; index < 2; ++index) slots[index] = std::move(other.slots[index]);
    signature = std::exchange(other.signature, 0);
    current = std::exchange(other.current, 0);
    geometryInputs = std::move(other.geometryInputs);
    candidateInputs = std::move(other.candidateInputs);
    geometryValid = std::exchange(other.geometryValid, false);
    return *this;
}
void DoubleBufferedMesh::Reset()
{
    for (auto& mesh : slots) {
        if (!mesh) continue;
        renderer::ReleaseOwnedResource(mesh->vertexBuffer);
        renderer::ReleaseOwnedResource(mesh->indexBuffer);
        mesh.reset();
    }
    signature = 0;
    current = 0;
    geometryValid = false;
    geometryInputs.clear();
    candidateInputs.clear();
}
} /// @note namespace fbzz::scene
