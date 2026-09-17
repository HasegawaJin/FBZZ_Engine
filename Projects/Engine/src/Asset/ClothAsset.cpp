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
}

bool ValidateClothAsset(const ClothAsset& asset)
{
    if (asset.vertices.empty() || asset.vertices.size() > CLOTH_MAX_VERTICES
        || asset.particles.empty() || asset.particles.size() > CLOTH_MAX_VERTICES
        || asset.indices.empty() || asset.indices.size() > CLOTH_MAX_INDICES
        || asset.indices.size() % 3 != 0 || asset.renderToParticle.size() != asset.vertices.size()
        || asset.pins.size() > asset.particles.size() || !ValidSkin(asset)) return false;
    for (const auto& p : asset.particles) if (!Finite(p)) return false;
    for (size_t i = 0; i < asset.vertices.size(); ++i) {
        const auto& v = asset.vertices[i];
        const uint32_t particle = asset.renderToParticle[i];
        if (particle >= asset.particles.size() || !Finite(v.position) || !Finite(v.normal) || !Finite(v.tangent)
            || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y)
            || !std::isfinite(v.color.x) || !std::isfinite(v.color.y) || !std::isfinite(v.color.z) || !std::isfinite(v.color.w)
            || (v.position - asset.particles[particle]).LengthSq() > 1.0e-12f) return false;
    }
    std::vector<uint32_t> triangles;
    std::vector<bool> used(asset.particles.size(), false), pinned(asset.particles.size(), false);
    for (uint32_t index : asset.indices) {
        if (index >= asset.vertices.size()) return false;
        const uint32_t particle = asset.renderToParticle[index];
        triangles.push_back(particle);
        used[particle] = true;
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
    if (version != 1 && version != CLOTH_FORMAT_VERSION) return false;
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
    if (version == CLOTH_FORMAT_VERSION) {
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
