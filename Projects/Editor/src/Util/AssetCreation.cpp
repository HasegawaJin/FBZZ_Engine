/// @file    AssetCreation.cpp
/// @brief   アセットの既定内容を1箇所で定義する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Util/AssetCreation.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Asset/SequenceAsset.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>

namespace fbzz::editor {
bool CreateEditorAsset(std::string_view kind, const std::string& path, const std::string& preset)
{
    if (path.empty() || util::FileSystem::Exists(path) || util::FileSystem::Exists(path + ".meta")) return false;
    if (kind == "folder") return util::FileSystem::EnsureDirectory(path);
    if (kind == "scene") return util::FileSystem::WriteTextAtomic(path, "# FBZZ Scene\n");
    if (kind == "material") {
        const char* materialTemplate =
            "version = 1\n"
            "shader = \"\"\n"
            "blend_mode = \"Opaque\"\n"
            "double_sided = false\n"
            "render_queue = 2000\n"
            "\n"
            "[textures]\n"
            "albedo = \"\"\n"
            "normal = \"\"\n"
            "metallic = \"\"\n"
            "roughness = \"\"\n"
            "ao = \"\"\n"
            "emissive = \"\"\n"
            "\n"
            "[params]\n"
            "base_color = [1.0, 1.0, 1.0, 1.0]\n"
            "metallic_factor = 0.0\n"
            "roughness_factor = 0.65\n"
            "normal_strength = 1.0\n"
            "emissive_color = [1.0, 1.0, 1.0]\n"
            "emissive_scale = 0.0\n";
        return util::FileSystem::WriteTextAtomic(path, materialTemplate);
    }
    if (kind == "vector_field") {
        fluid::VectorFieldAsset field;
        fluid::BakeVectorField(fluid::VectorFieldRecipe::Curl, 32, {5.0f, 5.0f, 5.0f}, 1, 1.0f, field);
        return asset::SaveVectorField(path, field);
    }
    if (kind == "fluid") return asset::SaveFluidRecipe(path, asset::MakeFluidPreset(asset::FluidPreset::Smoke));
    if (kind == "post_process_profile") return asset::DataAssetRegistry::Create(path, asset::PostProcessProfile::TYPE_NAME);
    if (kind == "animator_controller") return asset::SaveAnimatorControllerAsset(path, asset::AnimatorControllerAsset{});
    if (kind == "sequence") {
        asset::SequenceAsset sequence; sequence.name = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).stem()); sequence.duration = 3.0;
        asset::SequenceTrack cues; cues.type = asset::SequenceTrackType::Event; cues.name = "Cues";
        sequence.tracks.push_back(std::move(cues)); return asset::SaveSequenceAsset(path, sequence);
    }
    if (kind == "avatar_mask") {
        asset::AvatarMaskAsset mask; mask.name = "New Avatar Mask"; mask.defaultInclude = false;
        return asset::SaveAvatarMaskAsset(path, mask);
    }
    if (kind == "behavior_tree") {
        ai::BehaviorTreeAsset tree; ai::EnsureReservedBlackboardKeys(tree);
        ai::BTNodeDef root; root.id = 1; root.type = ai::BTNodeType::Selector; root.name = "Root";
        root.editorX = 80.0f; root.editorY = 80.0f; tree.nodes.push_back(root); tree.nextNodeId = 2;
        std::string error; return ai::SaveBehaviorTreeAsset(path, tree, &error);
    }
    if (kind == "physics_material") {
        for (int i = 0; i < physics::PhysicsMaterial::PRESET_COUNT; ++i) {
            if (preset != physics::PhysicsMaterial::PresetName(i)) continue;
            asset::PhysicsMaterialAsset material; material.material = *physics::PhysicsMaterial::PresetAt(i); material.presetName = preset;
            return asset::SavePhysicsMaterialAssetToFile(path, material);
        }
        return false;
    }
    if (kind == "synth") {
        for (int i = 0; i < static_cast<int>(audio::SynthPreset::Count); ++i) {
            const auto value = static_cast<audio::SynthPreset>(i);
            if (preset != audio::PresetToString(value)) continue;
            asset::SynthAsset synth; synth.spec = audio::MakePreset(value); synth.presetName = preset;
            return asset::SaveSynthAssetToFile(path, synth);
        }
        return false;
    }
    if (kind == "data_asset") {
        if (preset == asset::RenderPipelineAsset::TYPE_NAME) return asset::CreateRenderPipelineAsset(path, renderer::RenderSettings{});
        return asset::DataAssetRegistry::Create(path, preset);
    }
    return false;
}
} /// @note namespace fbzz::editor
