// FBZZ Engine
// PrimitiveMesh.cpp | fbzz::renderer
// 手続き生成メッシュの実装
// Cube / Sphere / Plane / Quad を CPU 側で生成し、ResourceManager で GPU バッファ化する。
// アセット読み込みなしでデバッグ・既定形状を使えるようにする。
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <Math/MathUtils.hpp>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

Mesh* PrimitiveMesh::Cube(ResourceManager& resources)
{
    static std::shared_ptr<Mesh> s_mesh;
    if (s_mesh) return s_mesh.get();
    // 6面 × 4頂点 = 24頂点。面ごとに法線が異なるため頂点共有なし
    // tangent は面の U 軸方向 (法線マップ対応。現在は旧シェーダーで未使用)
    const Vertex verts[] = {
        // +Y (top)  tangent = +X
        { .position={ -0.5f,  0.5f, -0.5f }, .normal={ 0, 1, 0 }, .tangent={ 1,0,0 }, .uv={ 0, 0 }},
        { .position={  0.5f,  0.5f, -0.5f }, .normal={ 0, 1, 0 }, .tangent={ 1,0,0 }, .uv={ 1, 0 }},
        { .position={  0.5f,  0.5f,  0.5f }, .normal={ 0, 1, 0 }, .tangent={ 1,0,0 }, .uv={ 1, 1 }},
        { .position={ -0.5f,  0.5f,  0.5f }, .normal={ 0, 1, 0 }, .tangent={ 1,0,0 }, .uv={ 0, 1 }},
        // -Y (bottom)  tangent = +X
        { .position={ -0.5f, -0.5f,  0.5f }, .normal={ 0,-1, 0 }, .tangent={ 1,0,0 }, .uv={ 0, 0 }},
        { .position={  0.5f, -0.5f,  0.5f }, .normal={ 0,-1, 0 }, .tangent={ 1,0,0 }, .uv={ 1, 0 }},
        { .position={  0.5f, -0.5f, -0.5f }, .normal={ 0,-1, 0 }, .tangent={ 1,0,0 }, .uv={ 1, 1 }},
        { .position={ -0.5f, -0.5f, -0.5f }, .normal={ 0,-1, 0 }, .tangent={ 1,0,0 }, .uv={ 0, 1 }},
        // +Z (front)  tangent = +X
        { .position={ -0.5f, -0.5f,  0.5f }, .normal={ 0, 0, 1 }, .tangent={ 1,0,0 }, .uv={ 0, 1 }},
        { .position={  0.5f, -0.5f,  0.5f }, .normal={ 0, 0, 1 }, .tangent={ 1,0,0 }, .uv={ 1, 1 }},
        { .position={  0.5f,  0.5f,  0.5f }, .normal={ 0, 0, 1 }, .tangent={ 1,0,0 }, .uv={ 1, 0 }},
        { .position={ -0.5f,  0.5f,  0.5f }, .normal={ 0, 0, 1 }, .tangent={ 1,0,0 }, .uv={ 0, 0 }},
        // -Z (back)  tangent = -X
        { .position={  0.5f, -0.5f, -0.5f }, .normal={ 0, 0,-1 }, .tangent={-1,0,0 }, .uv={ 0, 1 }},
        { .position={ -0.5f, -0.5f, -0.5f }, .normal={ 0, 0,-1 }, .tangent={-1,0,0 }, .uv={ 1, 1 }},
        { .position={ -0.5f,  0.5f, -0.5f }, .normal={ 0, 0,-1 }, .tangent={-1,0,0 }, .uv={ 1, 0 }},
        { .position={  0.5f,  0.5f, -0.5f }, .normal={ 0, 0,-1 }, .tangent={-1,0,0 }, .uv={ 0, 0 }},
        // +X (right)  tangent = -Z
        { .position={  0.5f, -0.5f,  0.5f }, .normal={ 1, 0, 0 }, .tangent={ 0,0,-1 }, .uv={ 0, 1 }},
        { .position={  0.5f, -0.5f, -0.5f }, .normal={ 1, 0, 0 }, .tangent={ 0,0,-1 }, .uv={ 1, 1 }},
        { .position={  0.5f,  0.5f, -0.5f }, .normal={ 1, 0, 0 }, .tangent={ 0,0,-1 }, .uv={ 1, 0 }},
        { .position={  0.5f,  0.5f,  0.5f }, .normal={ 1, 0, 0 }, .tangent={ 0,0,-1 }, .uv={ 0, 0 }},
        // -X (left)  tangent = +Z
        { .position={ -0.5f, -0.5f, -0.5f }, .normal={-1, 0, 0 }, .tangent={ 0,0, 1 }, .uv={ 0, 1 }},
        { .position={ -0.5f, -0.5f,  0.5f }, .normal={-1, 0, 0 }, .tangent={ 0,0, 1 }, .uv={ 1, 1 }},
        { .position={ -0.5f,  0.5f,  0.5f }, .normal={-1, 0, 0 }, .tangent={ 0,0, 1 }, .uv={ 1, 0 }},
        { .position={ -0.5f,  0.5f, -0.5f }, .normal={-1, 0, 0 }, .tangent={ 0,0, 1 }, .uv={ 0, 0 }},
    };

    // 面ごとに 2 三角形 (DX11: CW = 表面, FrontCounterClockwise=FALSE)
    // +Y/-Y は RHR が逆になるため巻き順を反転する
    const uint32_t idx[] = {
         0,  3,  2,   0,  2,  1,   // +Y (RHR=(0,+1,0) ✓)
         4,  7,  6,   4,  6,  5,   // -Y (RHR=(0,-1,0) ✓)
         8,  9, 10,   8, 10, 11,   // +Z
        12, 13, 14,  12, 14, 15,   // -Z
        16, 17, 18,  16, 18, 19,   // +X
        20, 21, 22,  20, 22, 23,   // -X
    };

    s_mesh = std::shared_ptr<Mesh>(new Mesh());
    s_mesh->vertexBuffer = resources.CreateVertexBuffer(verts, sizeof(verts), sizeof(Vertex));
    s_mesh->indexBuffer  = resources.CreateIndexBuffer(idx, 36);
    s_mesh->vertexCount  = 24;
    s_mesh->indexCount   = 36;
    s_mesh->cpuVertices.assign(std::begin(verts), std::end(verts));
    s_mesh->cpuIndices.assign(std::begin(idx), std::end(idx));
    s_mesh->ComputeBounds();
    return s_mesh.get();
}

Mesh* PrimitiveMesh::Sphere(ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();
    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;

    int rings = segments / 2;

    for (int r = 0; r <= rings; ++r) {
        float phi    = math::PI * r / rings;         // 0 → π
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int s = 0; s <= segments; ++s) {
            float theta    = math::TWO_PI * s / segments;  // 0 → 2π
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            math::Vector3 n = { sinPhi * cosTheta, cosPhi, sinPhi * sinTheta };
            // tangent = dPos/dTheta 方向 (正規化)
            math::Vector3 t = { -sinTheta, 0.0f, cosTheta };
            verts.push_back({
                .position = { n.x * 0.5f, n.y * 0.5f, n.z * 0.5f },
                .normal   = n,
                .tangent  = t,
                .uv       = { static_cast<float>(s) / segments, static_cast<float>(r) / rings }
            });
        }
    }

    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            uint32_t a = r       * (segments + 1) + s;
            uint32_t b = (r + 1) * (segments + 1) + s;
            idx.push_back(a); idx.push_back(a + 1); idx.push_back(b);
            idx.push_back(b); idx.push_back(a + 1); idx.push_back(b + 1);
        }
    }

    auto mesh = std::shared_ptr<Mesh>(new Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        verts.data(), verts.size() * sizeof(Vertex), sizeof(Vertex));
    mesh->indexBuffer  = resources.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    mesh->vertexCount  = static_cast<uint32_t>(verts.size());
    mesh->indexCount   = static_cast<uint32_t>(idx.size());
    mesh->cpuVertices  = verts;
    mesh->cpuIndices   = idx;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

Mesh* PrimitiveMesh::Plane(ResourceManager& resources)
{
    static std::shared_ptr<Mesh> s_mesh;
    if (s_mesh) return s_mesh.get();

    const Vertex verts[] = {
        { .position={ -0.5f, 0.0f,  0.5f }, .normal={ 0,1,0 }, .tangent={ 1,0,0 }, .uv={ 0, 0 }},
        { .position={  0.5f, 0.0f,  0.5f }, .normal={ 0,1,0 }, .tangent={ 1,0,0 }, .uv={ 1, 0 }},
        { .position={  0.5f, 0.0f, -0.5f }, .normal={ 0,1,0 }, .tangent={ 1,0,0 }, .uv={ 1, 1 }},
        { .position={ -0.5f, 0.0f, -0.5f }, .normal={ 0,1,0 }, .tangent={ 1,0,0 }, .uv={ 0, 1 }},
    };
    const uint32_t idx[] = { 0, 1, 2,  0, 2, 3 };

    s_mesh = std::shared_ptr<Mesh>(new Mesh());
    s_mesh->vertexBuffer = resources.CreateVertexBuffer(verts, sizeof(verts), sizeof(Vertex));
    s_mesh->indexBuffer  = resources.CreateIndexBuffer(idx, 6);
    s_mesh->vertexCount  = 4;
    s_mesh->indexCount   = 6;
    s_mesh->cpuVertices.assign(std::begin(verts), std::end(verts));
    s_mesh->cpuIndices.assign(std::begin(idx), std::end(idx));
    s_mesh->ComputeBounds();
    return s_mesh.get();
}

Mesh* PrimitiveMesh::Quad(ResourceManager& resources)
{
    static std::shared_ptr<Mesh> s_mesh;
    if (s_mesh) return s_mesh.get();

    const Vertex verts[] = {
        { .position={ -0.5f,  0.5f, 0.0f }, .normal={ 0,0,1 }, .tangent={ 1,0,0 }, .uv={ 0, 0 }},
        { .position={  0.5f,  0.5f, 0.0f }, .normal={ 0,0,1 }, .tangent={ 1,0,0 }, .uv={ 1, 0 }},
        { .position={  0.5f, -0.5f, 0.0f }, .normal={ 0,0,1 }, .tangent={ 1,0,0 }, .uv={ 1, 1 }},
        { .position={ -0.5f, -0.5f, 0.0f }, .normal={ 0,0,1 }, .tangent={ 1,0,0 }, .uv={ 0, 1 }},
    };
    const uint32_t idx[] = { 0, 1, 2,  0, 2, 3 };

    s_mesh = std::shared_ptr<Mesh>(new Mesh());
    s_mesh->vertexBuffer = resources.CreateVertexBuffer(verts, sizeof(verts), sizeof(Vertex));
    s_mesh->indexBuffer  = resources.CreateIndexBuffer(idx, 6);
    s_mesh->vertexCount  = 4;
    s_mesh->indexCount   = 6;
    s_mesh->cpuVertices.assign(std::begin(verts), std::end(verts));
    s_mesh->cpuIndices.assign(std::begin(idx), std::end(idx));
    s_mesh->ComputeBounds();
    return s_mesh.get();
}

Mesh* PrimitiveMesh::Cylinder(ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;

    constexpr float r = 0.5f;

    // Top cap (normal (0,1,0))
    // winding: (center, ring[s+1], ring[s]) → cross = (0,+1,0) ✓
    uint32_t topCenter = (uint32_t)verts.size();
    verts.push_back({ .position={ 0.0f, 0.5f, 0.0f }, .normal={ 0,1,0 }, .uv={ 0.5f, 0.5f }});
    uint32_t topRing = (uint32_t)verts.size();
    for (int s = 0; s < segments; ++s) {
        float t = math::TWO_PI * s / segments;
        float c = std::cos(t), si = std::sin(t);
        verts.push_back({ .position={ c*r, 0.5f, si*r }, .normal={ 0,1,0 }, .uv={ c*0.5f+0.5f, si*0.5f+0.5f }});
    }
    for (int s = 0; s < segments; ++s) {
        idx.push_back(topCenter);
        idx.push_back(topRing + (s + 1) % segments);
        idx.push_back(topRing + s);
    }

    // Bottom cap (normal (0,-1,0))
    // winding: (center, ring[s], ring[s+1]) → cross = (0,-1,0) ✓
    uint32_t botCenter = (uint32_t)verts.size();
    verts.push_back({ .position={ 0.0f, -0.5f, 0.0f }, .normal={ 0,-1,0 }, .uv={ 0.5f, 0.5f }});
    uint32_t botRing = (uint32_t)verts.size();
    for (int s = 0; s < segments; ++s) {
        float t = math::TWO_PI * s / segments;
        float c = std::cos(t), si = std::sin(t);
        verts.push_back({ .position={ c*r, -0.5f, si*r }, .normal={ 0,-1,0 }, .uv={ c*0.5f+0.5f, -si*0.5f+0.5f }});
    }
    for (int s = 0; s < segments; ++s) {
        idx.push_back(botCenter);
        idx.push_back(botRing + s);
        idx.push_back(botRing + (s + 1) % segments);
    }

    // Side (outward normal (cos t, 0, sin t), tangent = theta direction)
    // winding: (T_s, B_{s+1}, B_s) and (T_s, T_{s+1}, B_{s+1}) → cross = outward ✓
    uint32_t sideTop = (uint32_t)verts.size();
    for (int s = 0; s <= segments; ++s) {
        float t = math::TWO_PI * s / segments;
        float c = std::cos(t), si = std::sin(t);
        verts.push_back({ .position={ c*r, 0.5f, si*r }, .normal={ c,0,si },
                          .tangent={ -si, 0, c }, .uv={ (float)s/segments, 0.0f }});
    }
    uint32_t sideBot = (uint32_t)verts.size();
    for (int s = 0; s <= segments; ++s) {
        float t = math::TWO_PI * s / segments;
        float c = std::cos(t), si = std::sin(t);
        verts.push_back({ .position={ c*r, -0.5f, si*r }, .normal={ c,0,si },
                          .tangent={ -si, 0, c }, .uv={ (float)s/segments, 1.0f }});
    }
    for (int s = 0; s < segments; ++s) {
        uint32_t T0 = sideTop + s, T1 = sideTop + s + 1;
        uint32_t B0 = sideBot + s, B1 = sideBot + s + 1;
        idx.push_back(T0); idx.push_back(B1); idx.push_back(B0);
        idx.push_back(T0); idx.push_back(T1); idx.push_back(B1);
    }

    auto mesh = std::shared_ptr<Mesh>(new Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        verts.data(), verts.size() * sizeof(Vertex), sizeof(Vertex));
    mesh->indexBuffer  = resources.CreateIndexBuffer(idx.data(), (uint32_t)idx.size());
    mesh->vertexCount  = (uint32_t)verts.size();
    mesh->indexCount   = (uint32_t)idx.size();
    mesh->cpuVertices  = verts;
    mesh->cpuIndices   = idx;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

Mesh* PrimitiveMesh::Cone(ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;

    constexpr float r = 0.5f;
    // Slope normal at angle t: normalize(cos(t)*h, r, sin(t)*h) with h=1.0
    // |N| = sqrt(1 + rﾂｲ) = sqrt(1.25)
    constexpr float invSlopeLen = 1.0f / 1.118033989f; // 1/sqrt(1.25)

    // Side: per-segment triangle (apex, ring[s+1], ring[s])
    // winding: cross = outward ✓ (verified for θ_s=0, θ_{s+1}=π/2)
    for (int s = 0; s < segments; ++s) {
        float t0 = math::TWO_PI * s       / segments;
        float t1 = math::TWO_PI * (s + 1) / segments;
        float tm = (t0 + t1) * 0.5f;

        math::Vector3 apexN  = { std::cos(tm)*invSlopeLen, 0.5f*invSlopeLen, std::sin(tm)*invSlopeLen };
        math::Vector3 n0     = { std::cos(t0)*invSlopeLen, 0.5f*invSlopeLen, std::sin(t0)*invSlopeLen };
        math::Vector3 n1     = { std::cos(t1)*invSlopeLen, 0.5f*invSlopeLen, std::sin(t1)*invSlopeLen };

        uint32_t base = (uint32_t)verts.size();
        verts.push_back({ .position={ 0.0f,           0.5f,            0.0f           }, .normal=apexN,
                          .tangent={ -std::sin(tm), 0, std::cos(tm) }, .uv={ tm / math::TWO_PI, 0.0f }});
        verts.push_back({ .position={ std::cos(t0)*r, -0.5f, std::sin(t0)*r }, .normal=n0,
                          .tangent={ -std::sin(t0), 0, std::cos(t0) }, .uv={ t0 / math::TWO_PI, 1.0f }});
        verts.push_back({ .position={ std::cos(t1)*r, -0.5f, std::sin(t1)*r }, .normal=n1,
                          .tangent={ -std::sin(t1), 0, std::cos(t1) }, .uv={ t1 / math::TWO_PI, 1.0f }});

        idx.push_back(base); idx.push_back(base + 2); idx.push_back(base + 1);
    }

    // Bottom cap (normal (0,-1,0))
    uint32_t botCenter = (uint32_t)verts.size();
    verts.push_back({ .position={ 0.0f, -0.5f, 0.0f }, .normal={ 0,-1,0 }, .uv={ 0.5f, 0.5f }});
    uint32_t botRing = (uint32_t)verts.size();
    for (int s = 0; s < segments; ++s) {
        float t = math::TWO_PI * s / segments;
        float c = std::cos(t), si = std::sin(t);
        verts.push_back({ .position={ c*r, -0.5f, si*r }, .normal={ 0,-1,0 }, .uv={ c*0.5f+0.5f, -si*0.5f+0.5f }});
    }
    for (int s = 0; s < segments; ++s) {
        idx.push_back(botCenter);
        idx.push_back(botRing + s);
        idx.push_back(botRing + (s + 1) % segments);
    }

    auto mesh = std::shared_ptr<Mesh>(new Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        verts.data(), verts.size() * sizeof(Vertex), sizeof(Vertex));
    mesh->indexBuffer  = resources.CreateIndexBuffer(idx.data(), (uint32_t)idx.size());
    mesh->vertexCount  = (uint32_t)verts.size();
    mesh->indexCount   = (uint32_t)idx.size();
    mesh->cpuVertices  = verts;
    mesh->cpuIndices   = idx;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

Mesh* PrimitiveMesh::Torus(ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;

    int N = segments;       // major segments (around the big circle)
    int M = segments / 2;   // minor segments (around the tube)
    constexpr float R = 0.35f;  // major radius
    constexpr float r = 0.15f;  // tube radius  (R+r = 0.5 fits unit box)

    for (int n = 0; n <= N; ++n) {
        float phi    = math::TWO_PI * n / N;
        float cosPhi = std::cos(phi), sinPhi = std::sin(phi);

        for (int m = 0; m <= M; ++m) {
            float theta    = math::TWO_PI * m / M;
            float cosTheta = std::cos(theta), sinTheta = std::sin(theta);

            // tangent along the phi (major) direction
            math::Vector3 t = { -sinPhi, 0.0f, cosPhi };
            verts.push_back({
                .position = { (R + r * cosTheta) * cosPhi,  r * sinTheta,  (R + r * cosTheta) * sinPhi },
                .normal   = { cosTheta * cosPhi,             sinTheta,       cosTheta * sinPhi },
                .tangent  = t,
                .uv       = { (float)n / N,  (float)m / M }
            });
        }
    }

    // winding: (a, a+1, b) and (b, a+1, b+1) → cross = outward ✓
    for (int n = 0; n < N; ++n) {
        for (int m = 0; m < M; ++m) {
            uint32_t a  =  n      * (M + 1) + m;
            uint32_t b  = (n + 1) * (M + 1) + m;
            idx.push_back(a);   idx.push_back(a + 1); idx.push_back(b);
            idx.push_back(b);   idx.push_back(a + 1); idx.push_back(b + 1);
        }
    }

    auto mesh = std::shared_ptr<Mesh>(new Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        verts.data(), verts.size() * sizeof(Vertex), sizeof(Vertex));
    mesh->indexBuffer  = resources.CreateIndexBuffer(idx.data(), (uint32_t)idx.size());
    mesh->vertexCount  = (uint32_t)verts.size();
    mesh->indexCount   = (uint32_t)idx.size();
    mesh->cpuVertices  = verts;
    mesh->cpuIndices   = idx;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

Mesh* PrimitiveMesh::Capsule(ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;

    if (segments < 6) segments = 6;
    int N = segments;            // around
    int rings = std::max(2, segments / 2);
    int M = std::max(1, rings / 2); // hemisphere rings

    constexpr float rcap = 0.25f; // radius
    constexpr float h = 0.25f;    // half cylinder length

    // Top hemisphere (from pole to equator)
    uint32_t topHStart = (uint32_t)verts.size();
    for (int r = 0; r <= M; ++r) {
        float phi = (math::PI * 0.5f) * r / M; // 0 -> pi/2
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);
        for (int s = 0; s <= N; ++s) {
            float theta = math::TWO_PI * s / N;
            float cosT = std::cos(theta), sinT = std::sin(theta);
            math::Vector3 n = { sinPhi * cosT, cosPhi, sinPhi * sinT };
            verts.push_back({
                .position = { n.x * rcap, n.y * rcap + h, n.z * rcap },
                .normal   = n,
                .tangent  = { -sinT, 0.0f, cosT },
                .uv       = { (float)s / N, 1.0f - (float)r / M * 0.5f }
            });
        }
    }

    // Cylinder rings (top and bottom)
    uint32_t cylTopStart = (uint32_t)verts.size();
    for (int s = 0; s <= N; ++s) {
        float t = math::TWO_PI * s / N;
        float c = std::cos(t), si = std::sin(t);
        verts.push_back({ .position={ c*rcap, h, si*rcap }, .normal={ c,0,si },
                          .tangent={ -si, 0, c }, .uv={ (float)s / N, 0.5f } });
    }
    uint32_t cylBotStart = (uint32_t)verts.size();
    for (int s = 0; s <= N; ++s) {
        float t = math::TWO_PI * s / N;
        float c = std::cos(t), si = std::sin(t);
        verts.push_back({ .position={ c*rcap, -h, si*rcap }, .normal={ c,0,si },
                          .tangent={ -si, 0, c }, .uv={ (float)s / N, 0.5f + 0.5f } });
    }

    // Bottom hemisphere (from equator to pole)
    uint32_t botHStart = (uint32_t)verts.size();
    for (int r = 0; r <= M; ++r) {
        float phi = (math::PI * 0.5f) * r / M; // 0 -> pi/2
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);
        for (int s = 0; s <= N; ++s) {
            float theta = math::TWO_PI * s / N;
            float cosT = std::cos(theta), sinT = std::sin(theta);
            math::Vector3 n = { sinPhi * cosT, -cosPhi, sinPhi * sinT };
            verts.push_back({
                .position = { n.x * rcap, -cosPhi * rcap - h, n.z * rcap },
                .normal   = n,
                .tangent  = { -sinT, 0.0f, cosT },
                .uv       = { (float)s / N, 1.0f }
            });
        }
    }

    // Indices: top hemisphere
    for (int r = 0; r < M; ++r) {
        for (int s = 0; s < N; ++s) {
            uint32_t a = topHStart + r * (N + 1) + s;
            uint32_t b = topHStart + (r + 1) * (N + 1) + s;
            idx.push_back(a); idx.push_back(a + 1); idx.push_back(b);
            idx.push_back(b); idx.push_back(a + 1); idx.push_back(b + 1);
        }
    }

    // Indices: bottom hemisphere (pole -> equator)
    // For the bottom hemisphere the outward normal points downward, so we must
    // invert the winding compared to the top hemisphere to keep CW front faces.
    for (int r = 0; r < M; ++r) {
        for (int s = 0; s < N; ++s) {
            uint32_t a = botHStart + r * (N + 1) + s;
            uint32_t b = botHStart + (r + 1) * (N + 1) + s;
            // reversed order
            idx.push_back(b); idx.push_back(a + 1); idx.push_back(a);
            idx.push_back(b + 1); idx.push_back(a + 1); idx.push_back(b);
        }
    }

    // connect top hemisphere equator to cylinder top
    uint32_t topEquator = topHStart + M * (N + 1);
    for (int s = 0; s < N; ++s) {
        uint32_t a = topEquator + s;
        uint32_t b = cylTopStart + s;
        idx.push_back(a); idx.push_back(a + 1); idx.push_back(b);
        idx.push_back(b); idx.push_back(a + 1); idx.push_back(b + 1);
    }

    // cylinder sides
    for (int s = 0; s < N; ++s) {
        uint32_t T0 = cylTopStart + s, T1 = cylTopStart + s + 1;
        uint32_t B0 = cylBotStart + s, B1 = cylBotStart + s + 1;
        idx.push_back(T0); idx.push_back(B1); idx.push_back(B0);
        idx.push_back(T0); idx.push_back(T1); idx.push_back(B1);
    }

    // connect cylinder bottom to bottom hemisphere equator
    uint32_t botEquator = botHStart + M * (N + 1);
    for (int s = 0; s < N; ++s) {
        uint32_t a = cylBotStart + s;
        uint32_t b = botEquator + s;
        // match winding with bottom hemisphere (reverse)
        idx.push_back(b + 1); idx.push_back(a + 1); idx.push_back(a);
        idx.push_back(b);     idx.push_back(b + 1); idx.push_back(a);
    }

    auto mesh = std::shared_ptr<Mesh>(new Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        verts.data(), verts.size() * sizeof(Vertex), sizeof(Vertex));
    mesh->indexBuffer  = resources.CreateIndexBuffer(idx.data(), (uint32_t)idx.size());
    mesh->vertexCount  = (uint32_t)verts.size();
    mesh->indexCount   = (uint32_t)idx.size();
    mesh->cpuVertices  = verts;
    mesh->cpuIndices   = idx;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

} // namespace fbzz::renderer
