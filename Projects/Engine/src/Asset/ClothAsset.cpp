/// @file    ClothAsset.cpp
/// @brief   布アセットの検証、メッシュ変換と TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Format/ClothFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Physics/Cloth/ClothSolver.hpp>
#include <toml++/toml.hpp>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace fbzz::asset {
namespace {
bool Finite(const math::Vector3& p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool Affine(const math::Matrix4& matrix)
{
    for (const auto& row : matrix.m) for (float value : row) if (!std::isfinite(value)) return false;
    return std::abs(matrix.m[3][0]) < 1.0e-6f && std::abs(matrix.m[3][1]) < 1.0e-6f
        && std::abs(matrix.m[3][2]) < 1.0e-6f && std::abs(matrix.m[3][3] - 1.0f) < 1.0e-6f;
}
math::Vector3 Point(const math::Matrix4& matrix, const math::Vector3& position)
{
    const auto p = matrix * math::Vector4{position.x, position.y, position.z, 1.0f};
    return {p.x, p.y, p.z};
}
bool ValidSkin(const ClothAsset& asset)
{
    if (asset.skinBones.empty()) return asset.skinWeights.empty();
    if (asset.skinBones.size() > CLOTH_MAX_BONES || asset.skinWeights.size() != asset.particles.size()) return false;
    std::set<std::string> names;
    for (const auto& bone : asset.skinBones)
        if (bone.name.empty() || !names.insert(bone.name).second || !Affine(bone.inverseBind)) return false;
    for (const auto& skin : asset.skinWeights) {
        if (!Finite(skin.position)) return false;
        float sum = 0.0f;
        for (size_t i = 0; i < 4; ++i) {
            if (!std::isfinite(skin.weights[i]) || skin.weights[i] < 0.0f
                || skin.boneIndices[i] >= asset.skinBones.size()) return false;
            for (size_t j = 0; j < i; ++j)
                if (skin.weights[i] > 0.0f && skin.weights[j] > 0.0f && skin.boneIndices[i] == skin.boneIndices[j]) return false;
            sum += skin.weights[i];
        }
        if (std::abs(sum - 1.0f) > 1.0e-5f) return false;
    }
    return true;
}
bool ReadFloats(const toml::node& node, std::span<float> values)
{
    const auto* array = node.as_array();
    if (!array || array->size() != values.size()) return false;
    for (size_t i = 0; i < values.size(); ++i) {
        const auto value = (*array)[i].value<double>();
        if (!value || !std::isfinite(*value)) return false;
        values[i] = static_cast<float>(*value);
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}
bool ReadIndices(const toml::node* node, size_t limit, std::vector<uint32_t>& out)
{
    const auto* array = node ? node->as_array() : nullptr;
    if (!array || array->size() > limit) return false;
    for (const auto& entry : *array) {
        const auto value = entry.value<int64_t>();
        if (!value || *value < 0 || *value >= static_cast<int64_t>(CLOTH_MAX_VERTICES)) return false;
        out.push_back(static_cast<uint32_t>(*value));
    }
    return true;
}
toml::array IndexArray(const std::vector<uint32_t>& values)
{
    toml::array result;
    for (uint32_t value : values) result.push_back(static_cast<int64_t>(value));
    return result;
}
bool Frame(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c,
           math::Vector3& u, math::Vector3& v, math::Vector3& n)
{
    u = b - a;
    n = math::Vector3::Cross(u, c - a);
    if (!Finite(u) || !Finite(n) || u.LengthSq() < 1.0e-16f || n.LengthSq() < 1.0e-16f) return false;
    u = u.Normalized();
    n = n.Normalized();
    v = math::Vector3::Cross(n, u);
    return true;
}
/// @see https://realtimecollisiondetection.net/ Real-Time Collision Detection 5.1.5 三角形の Voronoi 領域による最近点。
math::Vector3 ClosestWeights(const math::Vector3& p, const math::Vector3& a,
                            const math::Vector3& b, const math::Vector3& c)
{
    using math::Vector3;
    const auto ab = b-a, ac = c-a, ap = p-a;
    const float d1 = Vector3::Dot(ab, ap), d2 = Vector3::Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return {1,0,0};
    const auto bp = p-b;
    const float d3 = Vector3::Dot(ab, bp), d4 = Vector3::Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return {0,1,0};
    const float vc = d1*d4-d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float t = d1/(d1-d3); return {1-t,t,0}; }
    const auto cp = p-c;
    const float d5 = Vector3::Dot(ab, cp), d6 = Vector3::Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return {0,0,1};
    const float vb = d5*d2-d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float t = d2/(d2-d6); return {1-t,0,t}; }
    const float va = d3*d6-d5*d4;
    if (va <= 0 && d4-d3 >= 0 && d5-d6 >= 0) {
        const float t = (d4-d3)/((d4-d3)+(d5-d6)); return {0,1-t,t};
    }
    const float sum = va+vb+vc;
    return {va/sum,vb/sum,vc/sum};
}
}

bool EvaluateClothRenderBindings(std::span<const ClothRenderBinding> bindings,
    std::span<const math::Vector3> rest, std::span<const math::Vector3> positions, std::vector<math::Vector3>& out)
{
    if (rest.size() != positions.size()) return false;
    std::vector<math::Vector3> result;
    result.reserve(bindings.size());
    for (const auto& binding : bindings) {
        const auto& ids = binding.particles;
        const auto& w = binding.barycentric;
        if (ids[0] >= positions.size() || ids[1] >= positions.size() || ids[2] >= positions.size()
            || !Finite(w) || !Finite(binding.offset) || w.x < 0 || w.y < 0 || w.z < 0
            || std::abs(w.x+w.y+w.z-1.0f) > 1.0e-5f) return false;
        const auto& a = positions[ids[0]]; const auto& b = positions[ids[1]]; const auto& c = positions[ids[2]];
        if (!Finite(a) || !Finite(b) || !Finite(c)) return false;
        math::Vector3 u, v, n;
        if (!Frame(a,b,c,u,v,n) && !Frame(rest[ids[0]],rest[ids[1]],rest[ids[2]],u,v,n)) return false;
        const auto p = a*w.x+b*w.y+c*w.z+u*binding.offset.x+v*binding.offset.y+n*binding.offset.z;
        if (!Finite(p)) return false;
        result.push_back(p);
    }
    out = std::move(result);
    return true;
}

bool BindClothRenderMesh(const ClothAsset& simulation, const renderer::Mesh& renderMesh, float maxDistance, ClothAsset& out)
{
    if (!ValidateClothAsset(simulation) || renderMesh.isSkinned || renderMesh.cpuVertices.empty()
        || !std::isfinite(maxDistance) || maxDistance < 0 || maxDistance > 100.0f) return false;
    ClothAsset result = simulation;
    if (result.renderBindings.empty()) {
        result.simulationIndices.clear();
        for (uint32_t index : result.indices) result.simulationIndices.push_back(result.renderToParticle[index]);
    }
    /// @note オフライン結合の総当たり上限。失敗しても元アセットや out を変更しない。
    if (renderMesh.cpuVertices.size() > CLOTH_MAX_VERTICES
        || renderMesh.cpuIndices.size() > CLOTH_MAX_INDICES
        || renderMesh.cpuVertices.size() * (result.simulationIndices.size()/3) > 20000000u) return false;
    result.vertices = renderMesh.cpuVertices;
    result.indices = renderMesh.cpuIndices;
    result.renderToParticle.clear();
    result.renderBindings.clear();
    for (const auto& vertex : result.vertices) {
        if (!Finite(vertex.position)) return false;
        ClothRenderBinding best;
        float bestDistance = maxDistance*maxDistance;
        bool found = false;
        for (size_t i = 0; i < result.simulationIndices.size(); i += 3) {
            const uint32_t ia = result.simulationIndices[i], ib = result.simulationIndices[i+1], ic = result.simulationIndices[i+2];
            const auto& a = result.particles[ia]; const auto& b = result.particles[ib]; const auto& c = result.particles[ic];
            const auto w = ClosestWeights(vertex.position,a,b,c);
            const auto delta = vertex.position-(a*w.x+b*w.y+c*w.z);
            const float distance = delta.LengthSq();
            if (!std::isfinite(distance) || distance > bestDistance || (found && distance == bestDistance)) continue;
            math::Vector3 u,v,n;
            if (!Frame(a,b,c,u,v,n)) return false;
            best = {{ia,ib,ic},w,{math::Vector3::Dot(delta,u),math::Vector3::Dot(delta,v),math::Vector3::Dot(delta,n)}};
            bestDistance = distance;
            found = true;
        }
        if (!found) return false;
        result.renderBindings.push_back(best);
    }
    result.revision = 0;
    if (!ValidateClothAsset(result)) return false;
    out = std::move(result);
    return true;
}

bool ValidateClothAsset(const ClothAsset& asset)
{
    const bool bound = !asset.renderBindings.empty();
    if (asset.vertices.empty() || asset.vertices.size() > CLOTH_MAX_VERTICES
        || asset.particles.empty() || asset.particles.size() > CLOTH_MAX_VERTICES
        || asset.indices.empty() || asset.indices.size() > CLOTH_MAX_INDICES
        || asset.indices.size() % 3 != 0
        || (bound ? (!asset.renderToParticle.empty() || asset.renderBindings.size() != asset.vertices.size()
            || asset.simulationIndices.empty() || asset.simulationIndices.size() > CLOTH_MAX_INDICES
            || asset.simulationIndices.size()%3 != 0)
            : (asset.renderToParticle.size() != asset.vertices.size() || !asset.simulationIndices.empty()))
        || asset.pins.size() > asset.particles.size() || !ValidSkin(asset)) return false;
    for (const auto& p : asset.particles) if (!Finite(p)) return false;
    for (size_t i = 0; i < asset.vertices.size(); ++i) {
        const auto& v = asset.vertices[i];
        const uint32_t particle = bound ? 0 : asset.renderToParticle[i];
        if (particle >= asset.particles.size() || !Finite(v.position) || !Finite(v.normal) || !Finite(v.tangent)
            || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y)
            || !std::isfinite(v.color.x) || !std::isfinite(v.color.y) || !std::isfinite(v.color.z) || !std::isfinite(v.color.w)
            || (!bound && (v.position - asset.particles[particle]).LengthSq() > 1.0e-12f)) return false;
    }
    std::vector<uint32_t> triangles;
    std::vector<bool> used(asset.particles.size(), false), pinned(asset.particles.size(), false);
    for (uint32_t index : asset.indices) {
        if (index >= asset.vertices.size()) return false;
        if (!bound) triangles.push_back(asset.renderToParticle[index]);
    }
    if (bound) {
        triangles = asset.simulationIndices;
        std::set<std::array<uint32_t,3>> faces;
        for (size_t i = 0; i < triangles.size(); i += 3) faces.insert({triangles[i],triangles[i+1],triangles[i+2]});
        for (const auto& binding : asset.renderBindings) if (!faces.contains(binding.particles)) return false;
        std::vector<math::Vector3> reconstructed;
        if (!EvaluateClothRenderBindings(asset.renderBindings, asset.particles, asset.particles, reconstructed)) return false;
        for (size_t i = 0; i < reconstructed.size(); ++i)
            if ((reconstructed[i]-asset.vertices[i].position).LengthSq() > 1.0e-8f) return false;
    }
    for (uint32_t particle : triangles) {
        if (particle >= used.size()) return false;
        used[particle] = true;
    }
    for (size_t i = 0; i < asset.indices.size(); i += 3) {
        const auto a = asset.indices[i], b = asset.indices[i+1], c = asset.indices[i+2];
        if (a == b || b == c || c == a || math::Vector3::Cross(asset.vertices[b].position-asset.vertices[a].position,
            asset.vertices[c].position-asset.vertices[a].position).LengthSq() < 1.0e-16f) return false;
    }
    for (bool value : used) if (!value) return false;
    for (uint32_t pin : asset.pins) {
        if (pin >= pinned.size() || pinned[pin]) return false;
        pinned[pin] = true;
    }
    /// @note 物理側の面積・共有辺検証を再利用し、インポートと実行で受理する形状を揃える。
    /// @see https://matthias-research.github.io/pages/publications/XPBD.pdf XPBD の三角形メッシュ拘束。
    physics::ClothSolver validator;
    return validator.Initialize(asset.particles, triangles, std::vector<float>(asset.particles.size(), 1.0f));
}

bool CreateClothAsset(const renderer::Mesh& mesh, std::span<const uint32_t> mapping, ClothAsset& out)
{
    if (mesh.isSkinned || mesh.cpuVertices.empty() || mesh.cpuVertices.size() > CLOTH_MAX_VERTICES
        || mesh.cpuIndices.size() > CLOTH_MAX_INDICES
        || (!mapping.empty() && mapping.size() != mesh.cpuVertices.size())) return false;
    ClothAsset result;
    result.vertices = mesh.cpuVertices;
    result.indices = mesh.cpuIndices;
    for (size_t i = 0; i < result.vertices.size(); ++i) {
        const uint32_t id = mapping.empty() ? static_cast<uint32_t>(i) : mapping[i];
        if (id >= result.vertices.size()) return false;
        result.renderToParticle.push_back(id);
        if (result.particles.size() <= id) result.particles.resize(static_cast<size_t>(id) + 1);
    }
    std::vector<bool> assigned(result.particles.size(), false);
    for (size_t i = 0; i < result.vertices.size(); ++i) {
        const uint32_t id = result.renderToParticle[i];
        if (!assigned[id]) result.particles[id] = result.vertices[i].position;
        assigned[id] = true;
    }
    if (!ValidateClothAsset(result)) return false;
    out = std::move(result);
    return true;
}

/// @see https://github.khronos.org/glTF-Tutorials/gltfTutorial/gltfTutorial_020_Skins.html inverse bind × 元頂点を各 joint の空間へ移す。
bool CreateSkinnedClothAsset(const renderer::Mesh& mesh, const Skeleton& skeleton,
                            std::span<const uint32_t> mapping, ClothAsset& out)
{
    if (!mesh.isSkinned || mesh.cpuSkinnedVertices.empty() || mesh.cpuSkinnedVertices.size() > CLOTH_MAX_VERTICES
        || mesh.cpuIndices.size() > CLOTH_MAX_INDICES || skeleton.bones.empty()
        || skeleton.bones.size() > CLOTH_MAX_BONES || skeleton.referencePose.size() != skeleton.bones.size()) return false;
    renderer::Mesh baked;
    baked.cpuIndices = mesh.cpuIndices;
    std::vector<ClothSkinWeight> skins;
    for (const auto& matrix : skeleton.referencePose) if (!Affine(matrix)) return false;
    for (const auto& vertex : mesh.cpuSkinnedVertices) {
        if (!Finite(vertex.position)) return false;
        std::map<uint32_t, double> weights;
        double sum = 0.0;
        for (size_t i = 0; i < 4; ++i) {
            const float weight = vertex.boneWeights[i];
            if (!std::isfinite(weight) || weight < 0.0f) return false;
            if (weight == 0.0f) continue;
            if (vertex.boneIndices[i] >= skeleton.bones.size()) return false;
            weights[vertex.boneIndices[i]] += weight;
            sum += weight;
        }
        if (sum <= 0.0) return false;
        ClothSkinWeight skin;
        skin.position = vertex.position;
        math::Vector3 position{};
        size_t slot = 0;
        for (const auto& [bone, weight] : weights) {
            skin.boneIndices[slot] = bone;
            skin.weights[slot] = static_cast<float>(weight / sum);
            position += Point(skeleton.referencePose[bone], vertex.position) * skin.weights[slot];
            ++slot;
        }
        skins.push_back(skin);
        baked.cpuVertices.push_back({position, vertex.normal, vertex.tangent, vertex.uv});
    }
    ClothAsset result;
    if (!CreateClothAsset(baked, mapping, result)) return false;
    for (const auto& bone : skeleton.bones) result.skinBones.push_back({bone.name, bone.offsetMatrix});
    result.skinWeights.resize(result.particles.size());
    std::vector<bool> assigned(result.particles.size(), false);
    for (size_t i = 0; i < skins.size(); ++i) {
        const uint32_t id = result.renderToParticle[i];
        if (assigned[id]) {
            const auto& previous = result.skinWeights[id];
            if ((previous.position - skins[i].position).LengthSq() > 1.0e-12f
                || previous.boneIndices != skins[i].boneIndices) return false;
            for (size_t slot = 0; slot < 4; ++slot)
                if (std::abs(previous.weights[slot] - skins[i].weights[slot]) > 1.0e-6f) return false;
        } else result.skinWeights[id] = skins[i];
        assigned[id] = true;
    }
    if (!ValidateClothAsset(result)) return false;
    out = std::move(result);
    return true;
}

bool EvaluateClothSkinning(const ClothAsset& asset, std::span<const math::Matrix4> boneWorld,
                          std::vector<math::Vector3>& out)
{
    if (asset.skinBones.empty() || boneWorld.size() != asset.skinBones.size() || !ValidSkin(asset)) return false;
    std::vector<math::Matrix4> palette;
    for (size_t i = 0; i < boneWorld.size(); ++i) {
        if (!Affine(boneWorld[i])) return false;
        palette.push_back(boneWorld[i] * asset.skinBones[i].inverseBind);
    }
    std::vector<math::Vector3> positions;
    positions.reserve(asset.skinWeights.size());
    for (const auto& skin : asset.skinWeights) {
        math::Vector3 position{};
        for (size_t i = 0; i < 4; ++i)
            if (skin.weights[i] > 0.0f) position += Point(palette[skin.boneIndices[i]], skin.position) * skin.weights[i];
        if (!Finite(position)) return false;
        positions.push_back(position);
    }
    out = std::move(positions);
    return true;
}

bool LoadClothAssetFromFile(std::string_view path, ClothAsset& out)
{
    const std::string filename(path);
    std::string text;
    if (!util::FileSystem::ReadText(filename, text)) return false;
    const auto parsed = toml::parse(text, filename);
    if (!parsed) return false;
    const auto& table = parsed.table();
    const auto version = table["version"].value<int64_t>();
    if (!version || *version < 1 || *version > CLOTH_FORMAT_VERSION) return false;
    const auto* particles = table["particles"].as_array();
    const auto* vertices = table["vertices"].as_array();
    if (!particles || !vertices || particles->size() > CLOTH_MAX_VERTICES || vertices->size() > CLOTH_MAX_VERTICES) return false;
    ClothAsset result;
    for (const auto& node : *particles) {
        float v[3]{};
        if (!ReadFloats(node, v)) return false;
        result.particles.push_back({v[0], v[1], v[2]});
    }
    for (const auto& node : *vertices) {
        float v[15]{};
        if (!ReadFloats(node, v)) return false;
        result.vertices.push_back({{v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]},
            {v[9], v[10]}, {v[11], v[12], v[13], v[14]}});
    }
    if (*version >= 2) {
        const auto* bones = table["skin_bones"].as_array();
        const auto* weights = table["skin_weights"].as_array();
        if (!bones || !weights || bones->size() > CLOTH_MAX_BONES || weights->size() > CLOTH_MAX_VERTICES) return false;
        for (const auto& node : *bones) {
            const auto* record = node.as_table();
            if (!record) return false;
            const auto name = (*record)["name"].value<std::string>();
            const auto* matrix = record->get("inverse_bind");
            float values[16]{};
            if (!name || !matrix || !ReadFloats(*matrix, values)) return false;
            ClothSkinBone bone;
            bone.name = *name;
            for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) bone.inverseBind.m[row][col] = values[row * 4 + col];
            result.skinBones.push_back(std::move(bone));
        }
        for (const auto& node : *weights) {
            const auto* record = node.as_table();
            if (!record) return false;
            ClothSkinWeight skin;
            float position[3]{};
            std::vector<uint32_t> indices;
            const auto* p = record->get("position");
            const auto* w = record->get("weights");
            if (!p || !w || !ReadFloats(*p, position) || !ReadFloats(*w, skin.weights)
                || !ReadIndices(record->get("bones"), 4, indices) || indices.size() != 4) return false;
            skin.position = {position[0], position[1], position[2]};
            std::copy(indices.begin(), indices.end(), skin.boneIndices.begin());
            result.skinWeights.push_back(skin);
        }
    }
    if (*version >= 3) {
        const auto* bindings = table["render_bindings"].as_array();
        if (!bindings || bindings->size() > CLOTH_MAX_VERTICES
            || !ReadIndices(table.get("simulation_indices"), CLOTH_MAX_INDICES, result.simulationIndices)) return false;
        for (const auto& node : *bindings) {
            const auto* record = node.as_table();
            if (!record) return false;
            std::vector<uint32_t> ids;
            float w[3]{}, offset[3]{};
            const auto* weights = record->get("barycentric");
            const auto* delta = record->get("offset");
            if (!ReadIndices(record->get("particles"), 3, ids) || ids.size() != 3 || !weights || !delta
                || !ReadFloats(*weights,w) || !ReadFloats(*delta,offset)) return false;
            result.renderBindings.push_back({{ids[0],ids[1],ids[2]},{w[0],w[1],w[2]},{offset[0],offset[1],offset[2]}});
        }
    }
    if (!ReadIndices(table.get("indices"), CLOTH_MAX_INDICES, result.indices)
        || !ReadIndices(table.get("render_to_particle"), CLOTH_MAX_VERTICES, result.renderToParticle)
        || !ReadIndices(table.get("pins"), CLOTH_MAX_VERTICES, result.pins) || !ValidateClothAsset(result)) {
        FBZZ_LOG_WARN("ClothAsset: invalid topology or mapping [%s]", filename.c_str());
        return false;
    }
    static std::atomic<uint64_t> nextRevision{1};
    result.revision = nextRevision.fetch_add(1, std::memory_order_relaxed);
    out = std::move(result);
    return true;
}

bool SaveClothAssetToFile(std::string_view path, const ClothAsset& asset)
{
    if (!ValidateClothAsset(asset)) return false;
    toml::table table;
    table.insert("version", CLOTH_FORMAT_VERSION);
    toml::array particles, vertices;
    for (const auto& p : asset.particles) particles.push_back(toml::array{p.x, p.y, p.z});
    for (const auto& v : asset.vertices) vertices.push_back(toml::array{
        v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
        v.tangent.x, v.tangent.y, v.tangent.z, v.uv.x, v.uv.y, v.color.x, v.color.y, v.color.z, v.color.w});
    table.insert("particles", std::move(particles));
    table.insert("vertices", std::move(vertices));
    table.insert("indices", IndexArray(asset.indices));
    table.insert("render_to_particle", IndexArray(asset.renderToParticle));
    table.insert("simulation_indices", IndexArray(asset.simulationIndices));
    toml::array bindings;
    for (const auto& binding : asset.renderBindings) {
        const auto& ids = binding.particles;
        const auto& w = binding.barycentric;
        const auto& d = binding.offset;
        bindings.push_back(toml::table{{"particles",toml::array{ids[0],ids[1],ids[2]}},
            {"barycentric",toml::array{w.x,w.y,w.z}}, {"offset",toml::array{d.x,d.y,d.z}}});
    }
    table.insert("render_bindings", std::move(bindings));
    table.insert("pins", IndexArray(asset.pins));
    toml::array bones, weights;
    for (const auto& bone : asset.skinBones) {
        toml::array matrix;
        for (const auto& row : bone.inverseBind.m) for (float value : row) matrix.push_back(value);
        bones.push_back(toml::table{{"name", bone.name}, {"inverse_bind", std::move(matrix)}});
    }
    for (const auto& skin : asset.skinWeights) {
        toml::array indices, values;
        for (uint32_t index : skin.boneIndices) indices.push_back(static_cast<int64_t>(index));
        for (float weight : skin.weights) values.push_back(weight);
        weights.push_back(toml::table{{"position", toml::array{skin.position.x, skin.position.y, skin.position.z}},
            {"bones", std::move(indices)}, {"weights", std::move(values)}});
    }
    table.insert("skin_bones", std::move(bones));
    table.insert("skin_weights", std::move(weights));
    std::ostringstream text;
    text << table;
    const std::string filename(path);
    return util::FileSystem::EnsureParentDirectory(util::FileSystem::PathFromUtf8(filename))
        && util::FileSystem::WriteTextAtomic(filename, text.str());
}
}
