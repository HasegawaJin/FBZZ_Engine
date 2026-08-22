// FBZZ Engine
// Mesh.hpp | fbzz::renderer
// GPU メッシュと頂点フォーマットの定義
// 静的メッシュとスキンメッシュの両方を扱う。
// CPU 側頂点配列はデバッグや再生成用に必要な範囲だけ保持する。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ResourceHandle.hpp"
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::renderer {

struct Vertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
};

struct SkinnedVertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
    uint32_t boneIndices[4] = {};
    float boneWeights[4] = {};
};

// DCC の BlendShape / Shape Key を基準頂点からの差分として保持する。
struct MorphTarget {
    std::string name;
    std::vector<math::Vector3> positionDeltas;
    std::vector<math::Vector3> normalDeltas;
    std::vector<math::Vector3> tangentDeltas;
};

struct Mesh {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    bool isSkinned = false;
    std::vector<Vertex>        cpuVertices;
    std::vector<SkinnedVertex> cpuSkinnedVertices;
    std::vector<uint32_t>      cpuIndices;
    std::vector<MorphTarget>   morphTargets;

    // フラスタム/オクルージョンカリング用バウンディング球 (ローカル空間)
    // ComputeBounds() を呼ぶと cpuVertices / cpuSkinnedVertices から算出される。
    // WHY: 球テストは AABB テストより分岐が少なく、ワールド変換も中心点変換+スケール乗算だけで済む。
    math::Vector3 boundsCenter  = {};
    float         boundsRadius  = 0.0f;
    // ローカル AABB の半分の大きさ (boundsCenter からの各軸の伸び)。
    // WHY 球と別に持つか: 球はカリング判定には十分だが、「この形は球で近似してよいか」を
    //     判定できない。板ポリと立方体は同じ半径の球になり得るのに、遮蔽者としての
    //     信頼度は正反対 (OcclusionCuller::TestAndRaster の内接正方形の前提が崩れる)。
    //     最小の半径成分と boundsRadius の比が、その判定材料になる。
    math::Vector3 boundsExtents = {};

    // CPU 頂点配列からバウンディング球と AABB を計算する。
    // メッシュ生成後・GPU アップロード前に一度だけ呼ぶ想定。
    void ComputeBounds();

    // boundsExtents だけを CPU 頂点配列から埋める (球には触れない)。
    // WHY: .mesh / .fzasset は球だけをヘッダーに持つため、そちらから読んだメッシュは
    //      AABB を知らないまま「薄い形」と区別がつかなくなる。フォーマットを変えずに
    //      補うため、頂点を読み終えた直後にこれだけを走らせる。
    void ComputeBoundsExtents();
};

} // namespace fbzz::renderer
