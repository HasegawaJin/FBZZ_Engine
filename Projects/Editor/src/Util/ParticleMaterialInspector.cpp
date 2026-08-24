/// @file    ParticleMaterialInspector.cpp
/// @brief   .mat の [particle] テーブル (パーティクルの «見た目») 用 Inspector セクション
/// @author  Hasegawa Jin
/// @date    2026-08-24

#include <Editor/Util/ParticleMaterialInspector.hpp>

#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FlipbookMotionVectors.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace fbzz::editor {
namespace {

struct ProceduralFlipbookUiState {
    int preset = 0;
    int frameSize = 128;
    int columns = 8;
    int rows = 2;
    int seed = 1;
    float noiseScale = 4.0f;
    float warpStrength = 0.65f;
    bool generateMotionVectors = false;
    std::string status;
    bool statusIsError = false;
};

// 生成ボタンの結果メッセージ。生成は数秒かかる同期処理で、押した直後に
// 何が起きたか分からないと不安になるため結果をパネルへ残す。
// Inspector は 1 つしか開かないため static で足りる。
std::string s_motionVectorStatus;
bool s_motionVectorStatusIsError = false;

// Project ごとの Generated 配下へ出し、Engine 同梱 Assets を誤って変更しない。
std::string ProceduralVFXOutputDirectory(const std::string& projectRoot)
{
    if (projectRoot.empty()) return "Assets/Textures/Generated/VFX";
    return projectRoot + "/Assets/Textures/Generated/VFX";
}

// テクスチャ参照を画像ローダーが開ける実パスへ変換する。
// WHY: [textures] は guid: 参照でも保存されるため、projectRoot を継ぎ足すだけでは開けない。
std::string ResolveTextureDiskPath(const std::string& projectRoot, const std::string& reference)
{
    if (reference.empty()) return {};
    if (std::string resolved = asset::AssetManager::ResolveAssetPath(reference); !resolved.empty())
        return resolved;
    return ToProjectAssetDiskPath(projectRoot, reference);
}

const std::string& TextureSlot(const asset::MaterialAsset& material, const char* slot)
{
    static const std::string kEmpty;
    const auto it = material.textures.find(slot);
    return it != material.textures.end() ? it->second : kEmpty;
}

} // namespace

bool DrawParticleMaterialInspector(asset::MaterialAsset& material, const std::string& projectRoot)
{
    auto& particle = material.particle;
    bool changed = false;

    ImGui::PushID("particle_material");

    ImGui::SeparatorText("Particle / Alpha");
    {
        // テクスチャの作りの違いを吸収する。素材を画像編集ソフトで加工させないための設定。
        // 並びは Rendering/Mask.hlsli の FBZZ_MASK_* と一致させること。
        static constexpr const char* kAlphaItems[] = {
            "Texture Alpha", "Luminance (black = clear)",
            "Inverted Luminance (white = clear)", "Red Channel",
            "Green Channel", "Blue Channel", "Inverted Alpha" };
        int alphaSource = static_cast<int>(particle.alphaSource);
        if (ImGui::Combo("Alpha Source", &alphaSource, kAlphaItems, IM_ARRAYSIZE(kAlphaItems))) {
            particle.alphaSource = static_cast<scene::ParticleAlphaSource>(alphaSource);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "テクスチャのどこを「不透明度」として読むかを選びます。\n\n"
                "Texture Alpha : アルファ付き素材 (通常の PNG/TGA)\n"
                "Luminance     : 黒背景でアルファが無い素材。明るいほど濃く出ます\n"
                "Inverted      : 白背景の素材。暗いほど濃く出ます\n"
                "R/G/B Channel : 1枚に複数のマスクを詰めたパック済み素材\n"
                "Inverted Alpha: アルファの意味が逆になっている素材\n\n"
                "素材を Alpha Blend にしたら黒い四角が出る場合は Luminance を選びます。\n"
                "この選択肢は全マテリアル共通の語彙です (Rendering/Mask.hlsli)。");
        if (particle.alphaSource == scene::ParticleAlphaSource::TextureAlpha
            && material.blendMode != renderer::BlendMode::ADDITIVE)
            ImGui::TextDisabled("黒い矩形が出る場合はアルファ無し素材です。Luminance を試してください。");

        changed |= ImGui::DragFloat("HDR Emissive", &particle.emissiveScale, 0.01f, 0.0f, 100.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("粒子色に掛かる倍率。1 を超えるとブルームが拾います。");

        changed |= ImGui::Checkbox("Soft Particles", &particle.softParticles);
        if (particle.softParticles)
            changed |= ImGui::DragFloat("Soft Fade Distance", &particle.softParticleFadeDistance,
                                        0.01f, 0.001f, 100.0f);
    }

    ImGui::SeparatorText("Texture Sheet Animation");
    {
        changed |= ImGui::DragInt("Columns", &particle.spriteColumns, 1, 1, 64);
        changed |= ImGui::DragInt("Rows", &particle.spriteRows, 1, 1, 64);
        changed |= ImGui::DragInt("Start Frame", &particle.spriteStartFrame, 1, 0, 4095);
        changed |= ImGui::DragInt("End Frame", &particle.spriteEndFrame, 1, 0, 4095);
        static constexpr const char* kFlipbookItems[] = { "Lifetime", "FPS", "Random", "Ping Pong" };
        int flipbookMode = static_cast<int>(particle.flipbookMode);
        if (ImGui::Combo("Mode", &flipbookMode, kFlipbookItems, IM_ARRAYSIZE(kFlipbookItems))) {
            particle.flipbookMode = static_cast<scene::ParticleFlipbookMode>(flipbookMode);
            changed = true;
        }
        if (particle.flipbookMode != scene::ParticleFlipbookMode::Lifetime
            && particle.flipbookMode != scene::ParticleFlipbookMode::RandomFrame)
            changed |= ImGui::DragFloat("FPS", &particle.flipbookFramesPerSecond, 0.1f, 0.0f, 240.0f);
        changed |= ImGui::Checkbox("Frame Blending", &particle.flipbookFrameBlending);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("隣接コマを線形補間します。\n"
                              "有効にすると GPU シミュレーションは使えません (CPU へ縮退)。");
        changed |= ImGui::Checkbox("Random Start Frame", &particle.spriteRandomStartFrame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("粒子ごとに再生位相をずらします。\n"
                              "同時に湧いた煙が全部同じコマで回って一枚板に見えるのを防ぎます。");
        changed |= ImGui::Checkbox("Random Row", &particle.spriteRandomRow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("アトラスの各行を別バリエーションとして扱い、粒子ごとに1行を選びます。\n"
                              "1枚のアトラスで見た目の異なる煙・爆炎を混ぜられます。\n"
                              "有効時は Start/End Frame より行の範囲が優先されます。");

        if (ImGui::TreeNode("Procedural Flipbook Generator")) {
            // 関数ローカル static ならグローバル状態を増やさず、Inspector を閉じても設定を保持できる。
            static ProceduralFlipbookUiState procedural;
            static constexpr const char* kPresetNames[] = { "Smoke", "Fire", "Explosion", "Distortion" };
            ImGui::Combo("Preset", &procedural.preset, kPresetNames, IM_ARRAYSIZE(kPresetNames));
            ImGui::DragInt("Frame Size", &procedural.frameSize, 8.0f, 32, 512);
            ImGui::DragInt("Frames", &procedural.columns, 1.0f, 2, 32);
            ImGui::DragInt("Variants", &procedural.rows, 1.0f, 1, 16);
            ImGui::DragInt("Seed", &procedural.seed, 1.0f, 0, 1000000);
            ImGui::DragFloat("Noise Scale", &procedural.noiseScale, 0.05f, 0.25f, 32.0f);
            ImGui::DragFloat("Warp Strength", &procedural.warpStrength, 0.01f, 0.0f, 3.0f);
            const bool distortionPreset = procedural.preset
                == static_cast<int>(asset::ProceduralFlipbookPreset::Distortion);
            ImGui::BeginDisabled(distortionPreset);
            ImGui::Checkbox("Generate Motion Vectors", &procedural.generateMotionVectors);
            ImGui::EndDisabled();
            if (distortionPreset)
                ImGui::TextDisabled("Distortion は RG 自体が変位なので Motion Vector を生成しません。");
            ImGui::TextDisabled("生成した PNG をこの .mat の albedo へ割り当て、\n"
                                "プリセットに対応するブレンドとフリップブック設定を焼きます。");

            if (ImGui::Button("Generate & Assign", { -1.0f, 0.0f })) {
                asset::ProceduralFlipbookSettings settings;
                settings.preset = static_cast<asset::ProceduralFlipbookPreset>(procedural.preset);
                settings.frameSize = procedural.frameSize;
                settings.columns = procedural.columns;
                settings.rows = procedural.rows;
                settings.seed = static_cast<std::uint32_t>((std::max)(procedural.seed, 0));
                settings.noiseScale = procedural.noiseScale;
                settings.warpStrength = procedural.warpStrength;
                const auto result = asset::GenerateProceduralFlipbook(
                    ProceduralVFXOutputDirectory(projectRoot), settings);
                procedural.status = result.message;
                procedural.statusIsError = !result.success;
                if (result.success) {
                    material.textures["albedo"] = NormalizeAssetPath(result.albedoPath);
                    particle.spriteColumns = settings.columns;
                    particle.spriteRows = settings.rows;
                    particle.spriteStartFrame = 0;
                    particle.spriteEndFrame = settings.columns * settings.rows - 1;
                    particle.flipbookFrameBlending = true;
                    particle.spriteRandomStartFrame = false;
                    particle.spriteRandomRow = settings.rows > 1;
                    particle.motionVectorFlipbook = false;
                    material.textures["tex5"].clear();

                    if (distortionPreset) {
                        particle.flipbookMode = scene::ParticleFlipbookMode::FramesPerSecond;
                        particle.flipbookFramesPerSecond = 24.0f;
                        material.blendMode = renderer::BlendMode::ALPHA_BLEND;
                        particle.distortion = true;
                    } else {
                        particle.flipbookMode = scene::ParticleFlipbookMode::Lifetime;
                        particle.distortion = false;
                        if (settings.preset == asset::ProceduralFlipbookPreset::Fire)
                            material.blendMode = renderer::BlendMode::ADDITIVE;
                        else if (settings.preset == asset::ProceduralFlipbookPreset::Explosion)
                            material.blendMode = renderer::BlendMode::PREMULTIPLIED;
                        else
                            material.blendMode = renderer::BlendMode::ALPHA_BLEND;

                        if (procedural.generateMotionVectors) {
                            asset::FlipbookMotionVectorSettings mvSettings;
                            mvSettings.columns = settings.columns;
                            mvSettings.rows = settings.rows;
                            mvSettings.loop = false;
                            mvSettings.rowSequences = settings.rows > 1;
                            const auto mvResult = asset::GenerateFlipbookMotionVectors(
                                result.albedoPath, mvSettings);
                            if (mvResult.success) {
                                particle.motionVectorFlipbook = true;
                                material.textures["tex5"] = NormalizeAssetPath(mvResult.outputPath);
                                procedural.status += "\n" + mvResult.message;
                            } else {
                                procedural.status += "\nMV生成失敗: " + mvResult.message;
                                procedural.statusIsError = true;
                            }
                        }
                    }
                    changed = true;
                }
            }
            if (!procedural.status.empty()) {
                if (procedural.statusIsError)
                    ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", procedural.status.c_str());
                else
                    ImGui::TextWrapped("%s", procedural.status.c_str());
            }
            ImGui::TreePop();
        }

        changed |= ImGui::Checkbox("Motion Vector Blending", &particle.motionVectorFlipbook);
        if (particle.motionVectorFlipbook) {
            // アトラスは [textures] tex5。ParticlePass が同じスロットから読む。
            changed |= widgets::AssetPathField("Motion Vector Atlas (tex5)", material.textures["tex5"],
                                               widgets::kTextureAssetFilter, projectRoot);
            changed |= ImGui::DragFloat("Motion Strength", &particle.motionVectorStrength,
                                        0.01f, 0.0f, 8.0f);

            // MV アトラスは外部ツールでしか作れず「機能はあるのに使えない」状態だったため、
            // 現在の albedo から生成してそのまま割り当てられるようにする。
            ImGui::Separator();
            const std::string atlasPath =
                ResolveTextureDiskPath(projectRoot, TextureSlot(material, "albedo"));
            ImGui::BeginDisabled(atlasPath.empty());
            if (ImGui::Button("Generate From Texture", { -1.0f, 0.0f })) {
                asset::FlipbookMotionVectorSettings mvSettings;
                mvSettings.columns = particle.spriteColumns;
                mvSettings.rows = particle.spriteRows;
                mvSettings.loop = particle.flipbookMode == scene::ParticleFlipbookMode::FramesPerSecond
                    || particle.spriteRandomStartFrame;
                mvSettings.rowSequences = particle.spriteRandomRow;
                const auto result = asset::GenerateFlipbookMotionVectors(atlasPath, mvSettings);
                s_motionVectorStatus = result.message;
                s_motionVectorStatusIsError = !result.success;
                if (result.success) {
                    // 生成結果をそのまま割り当てる。手で貼り直す手間を残さない。
                    material.textures["tex5"] = NormalizeAssetPath(result.outputPath);
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            if (atlasPath.empty())
                ImGui::TextDisabled("albedo を設定すると生成できます。");
            if (!s_motionVectorStatus.empty()) {
                if (s_motionVectorStatusIsError)
                    ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", s_motionVectorStatus.c_str());
                else
                    ImGui::TextWrapped("%s", s_motionVectorStatus.c_str());
            }
        }
    }

    ImGui::SeparatorText("Distortion");
    {
        changed |= ImGui::Checkbox("Distortion / Heat Haze", &particle.distortion);
        if (particle.distortion) {
            changed |= ImGui::DragFloat("Distortion Strength", &particle.distortionStrength,
                                        0.001f, 0.0f, 0.25f, "%.4f");
            // 歪み専用マップは [textures] normal。未設定なら albedo の RG へ縮退する。
            changed |= widgets::AssetPathField("Distortion Map (normal)", material.textures["normal"],
                                               widgets::kTextureAssetFilter, projectRoot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("歪みの向きを決める RG マップです。\n"
                                  "未設定なら albedo の RG を流用しますが、\n"
                                  "その場合は素材を差し替えると曲がる向きも変わります。");
            changed |= ImGui::DragFloat("Chromatic Aberration", &particle.distortionChromatic,
                                        0.01f, 0.0f, 4.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("屈折率の波長依存を RGB のずれで表します。\n"
                                  "衝撃波の縁が単色で滑るのを防ぎます。");
        }
    }

    ImGui::SeparatorText("Smoke Lighting");
    {
        if (ImGui::Checkbox("Six-way Lit Smoke", &particle.sixWayLighting)) {
            // 役割が重複するため排他にする (両方掛けると二重に陰影が付いて濁る)。
            if (particle.sixWayLighting) particle.volumetric = false;
            changed = true;
        }
        if (particle.sixWayLighting) {
            // シェーダー側が saturate するため 1.0 が上限。それ以上は「元の色を捨てて
            // (ambient + N·L) で塗る」だけになり、暗い環境で煙が真っ黒に潰れる。
            changed |= ImGui::SliderFloat("Lighting Strength", &particle.lightingStrength, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = 元の色そのまま / 1 = 完全にライティングで置換");
            changed |= ImGui::SliderFloat("Smoke Wrap", &particle.smokeWrap, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("陰側への光の回り込みです。\n"
                                  "0 だと不透明な球の陰影になり、明暗の境界が硬く黒く落ちます。");
            changed |= ImGui::DragFloat("Smoke Transmission", &particle.smokeTransmission,
                                        0.01f, 0.0f, 8.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("逆光透過 (前方散乱) の強さです。\n"
                                  "光源が煙の向こうにあるとき、縁が光って厚みが見えます。\n"
                                  "0 だと炎が煙の背後にあっても煙は暗いままです。");
            if (particle.smokeTransmission > 0.0f)
                changed |= ImGui::DragFloat("Back Scatter Power", &particle.smokeBackScatterPower,
                                            0.1f, 0.1f, 64.0f);
        }

        if (ImGui::Checkbox("Volumetric Smoke", &particle.volumetric)) {
            if (particle.volumetric) particle.sixWayLighting = false;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ビルボード内で密度場をレイマーチして厚みを出します。\n"
                              "カメラが回り込んでも「紙が回った」ように見えません。\n"
                              "テクスチャは使わず密度場で色を作るため Six-way とは排他です。");
        if (particle.volumetric) {
            changed |= ImGui::DragInt("Volumetric Steps", &particle.volumetricSteps, 1, 1, 64);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("1 ピクセルあたりのループ回数です。増やすほど滑らかですが重くなります。");
            changed |= ImGui::DragFloat("Density", &particle.volumetricDensity, 0.01f, 0.0f, 20.0f);
            changed |= ImGui::SliderFloat("Anisotropy", &particle.volumetricAnisotropy, -0.95f, 0.95f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("正で前方散乱。逆光のとき煙の縁が光ります。");
            changed |= ImGui::DragFloat("Noise Scale", &particle.volumetricNoiseScale, 0.05f, 0.0f, 32.0f);
            if (material.blendMode == renderer::BlendMode::ADDITIVE)
                ImGui::TextDisabled("出力は事前乗算アルファです。Premultiplied ブレンドを推奨します。");
        }
    }

    ImGui::SeparatorText("Shadows");
    {
        changed |= ImGui::Checkbox("Receive Shadows", &particle.receiveShadows);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("影の中で粒子を暗くします。\n"
                              "煙・埃が背景から浮いて見える最大の原因がこれです。\n"
                              "発光エフェクト(加算)では通常オフのままにします。");
        if (particle.receiveShadows) {
            changed |= ImGui::SliderFloat("Shadow Strength", &particle.shadowStrength, 0.0f, 1.0f);
            if (material.blendMode == renderer::BlendMode::ADDITIVE)
                ImGui::TextDisabled("加算ブレンドでは影が暗くしても見えにくくなります。"
                                    "煙は Alpha / Premultiplied を推奨。");
        }
        // 自己影。受け影とは別の現象なので、別のスライダーとして並べる。
        changed |= ImGui::DragFloat("Self Shadow", &particle.selfShadowStrength,
                                    0.01f, 0.0f, 8.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "粒子群が自分自身へ落とす影の濃さ (0 で無効)。\n"
                "受け影は「他の物体が落とす影」だけを扱うため、これが無いと\n"
                "粒子をいくら重ねても光の当たり方が一様で、厚みのある煙・雲が平坦に見えます。\n"
                "有効にすると光源から見た密度を積む追加パスが走り、GPU シミュレーションは使えません。");
        if (particle.selfShadowStrength > 0.0f)
            ImGui::TextDisabled("光源側の密度から Beer-Lambert 則で減衰させる近似です。"
                                "厚みの表現が目的で、物理的な正確さは狙っていません。");
    }

    ImGui::PopID();
    return changed;
}

} // namespace fbzz::editor
