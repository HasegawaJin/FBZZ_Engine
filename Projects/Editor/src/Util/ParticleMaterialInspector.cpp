/// @file    ParticleMaterialInspector.cpp
/// @brief   .mat の [particle] テーブル (パーティクルの «見た目») 用 Inspector セクション
/// @author  Hasegawa Jin
/// @date    2026-08-24

#include <Editor/Util/ParticleMaterialInspector.hpp>

#include <Editor/Util/FlipbookInspector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <imgui.h>
#include <string>

namespace fbzz::editor {

bool DrawParticleMaterialInspector(asset::MaterialAsset& material, const std::string& projectRoot,
                                   renderer::ResourceManager* resources,
                                   renderer::IImGuiRenderer* imguiRenderer)
{
    auto& particle = material.particle;
    bool changed = false;

    ImGui::PushID("particle_material");

    ImGui::SeparatorText("Particle / Alpha");
    {
        /// @note テクスチャの作りの違いを吸収する。素材を画像編集ソフトで加工させないための設定。
        ///       並びは Rendering/Mask.hlsli の FBZZ_MASK_* と一致させること。
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

        changed |= ImGui::DragFloat("Camera Fade Near##pm_camera_fade_near", &particle.cameraFadeNear,
                                    0.01f, 0.0f, 100.0f);
        changed |= ImGui::DragFloat("Camera Fade Far##pm_camera_fade_far", &particle.cameraFadeFar,
                                    0.01f, 0.0f, 100.0f);
        if (particle.cameraFadeNear >= particle.cameraFadeFar)
            ImGui::TextDisabled("Near >= Far のあいだは無効 (カメラ距離で薄めません)。");
        else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Near より近い粒子を薄くする。カメラが煙に突っ込んで画面が埋まるのを防ぐ。");
    }

    changed |= DrawFlipbookInspector(material, projectRoot, resources, imguiRenderer);

    ImGui::SeparatorText("Distortion");
    {
        changed |= ImGui::Checkbox("Distortion / Heat Haze", &particle.distortion);
        if (particle.distortion) {
            changed |= ImGui::DragFloat("Distortion Strength", &particle.distortionStrength,
                                        0.001f, 0.0f, 0.25f, "%.4f");
            /// @note 歪み専用マップは [textures] normal。未設定なら albedo の RG へ縮退する。
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
            /// @note 役割が重複するため排他にする (両方掛けると二重に陰影が付いて濁る)。
            if (particle.sixWayLighting) particle.volumetric = false;
            changed = true;
        }
        if (particle.sixWayLighting) {
            /// @note シェーダー側が saturate するため 1.0 が上限。それ以上は「元の色を捨てて
            ///       (ambient + N·L) で塗る」だけになり、暗い環境で煙が真っ黒に潰れる。
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

        changed |= ImGui::Checkbox("Six-way Lightmaps", &particle.sixWayMaps);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Volume Flipbook Baker の 6-way 出力で陰影を付けます。\n"
                              "albedo = _6wayP (右/上/奥/α)、emissive = _6wayN (左/下/手前/発光)。\n"
                              "光を回すと、明暗が煙の形に沿って回り込みます。");
        if (particle.sixWayMaps) {
            const auto negative = material.textures.find("emissive");
            if (negative == material.textures.end() || negative->second.empty())
                ImGui::TextColored({ 1.0f, 0.65f, 0.3f, 1.0f }, "emissive スロットに _6wayN を割り当ててください");
            changed |= ImGui::ColorEdit3("Six-way Emission", &particle.sixWayEmissionColor.x,
                                         ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("_6wayN の A (発光マスク) に掛ける色です。炎の芯は影の中でも光ります。");
        }
        changed |= ImGui::Checkbox("Receive Point Lights", &particle.punctualLighting);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Point / Spot / 面光源を粒子の中心で受けます (松明の横の煙が照らされる)。\n"
                              "画素ごとにライト一覧を走査するので、画面を覆う煙では重くなります。");

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
        /// @note 自己影。受け影とは別の現象なので、別のスライダーとして並べる。
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
