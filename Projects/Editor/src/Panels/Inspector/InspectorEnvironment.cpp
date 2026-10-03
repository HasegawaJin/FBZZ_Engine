/// @file    InspectorEnvironment.cpp
/// @brief   Environment / Decal / IBL / PostProcess 系 Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorEnvironment.hpp"

namespace fbzz::editor {

void DrawEnvironmentInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::SkyRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sky Renderer",
        [](scene::SkyRenderer& sr, EditorContext&) {
            ImGui::SeparatorText("Atmosphere");
            widgets::DragVec3("Rayleigh", sr.rayleighScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Mie Scattering", &sr.mieScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Sky Scatter Intensity", &sr.skyScatterIntensity, 0.1f, 0.0f, 1000.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("大気散乱そのものの明るさ。太陽ディスクの明るさは\n"
                                  "Sun Moon Renderer の Sun Disk Intensity です。");
            ImGui::DragFloat("Planet Radius", &sr.planetRadius, 1.0f, 1.0f, 100000.0f);
            ImGui::DragFloat("Atmosphere Radius", &sr.atmosphereRadius, 1.0f, 1.0f, 100000.0f);
            widgets::RangeField("Mie G", sr.mieG, -0.99f, 0.99f);

            ImGui::SeparatorText("Day Night");
            ImGui::Checkbox("Day Night Enabled", &sr.dayNightEnabled);
            if (sr.dayNightEnabled) {
                /// @note DayNight 有効時の Color / Intensity は空が上書きし、ライトは向きだけを指定する。
                ImGui::TextDisabled("太陽の向きは Directional Light の Transform。\n"
                                    "有効な間、そのライトの Color / Intensity はここが決めます。");

                ImGui::DragFloat("Day Altitude", &sr.dayAltitude, 0.5f, 0.1f, 90.0f, "%.1f\xc2\xb0");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("太陽がこの高度まで昇ると完全な昼になります。\n"
                                      "地平線 (0°) からここまでが夕方の帯です。");
                ImGui::DragFloat("Night Altitude", &sr.nightAltitude, 0.5f, 0.1f, 90.0f, "-%.1f\xc2\xb0");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("太陽が地平線からこの角度だけ沈むと完全な夜になります。\n"
                                      "地平線 (0°) からここまでが薄明の帯です。");

                widgets::ColorEdit3("Day Color", sr.dayColor);
                widgets::ColorEdit3("Sunset Color", sr.sunsetColor);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("太陽高度がちょうど 0° のときの色。\n"
                                      "夕方を作るには Directional Light を水平へ向けます。");
                widgets::ColorEdit3("Night Color", sr.nightColor);

                /// @note Intensity=1 は白い拡散面が albedo の明るさになる単位 (LIGHT_UNIT_SCALE)。
                ImGui::DragFloat("Day Intensity", &sr.dayIntensity, 0.01f, 0.0f, 20.0f);
                ImGui::DragFloat("Sunset Intensity", &sr.sunsetIntensity, 0.01f, 0.0f, 20.0f);
                ImGui::DragFloat("Night Intensity", &sr.nightIntensity, 0.01f, 0.0f, 5.0f);

                /// @note 空の倍率は地表への太陽光と独立し、Sky Scatter Intensity に乗算する。
                ImGui::DragFloat("Sky Day Brightness", &sr.skyDayBrightness, 0.01f, 0.0f, 20.0f);
                ImGui::DragFloat("Sky Sunset Brightness", &sr.skySunsetBrightness, 0.01f, 0.0f, 20.0f);
                ImGui::DragFloat("Sky Night Brightness", &sr.skyNightBrightness, 0.01f, 0.0f, 5.0f);
            }

            ImGui::SeparatorText("Cloud Shadow");
            widgets::RangeField("Cloud Shadow Strength", sr.cloudShadowStrength, 0.0f, 1.0f);
            widgets::RangeField("Cloud Shadow Coverage", sr.cloudShadowCoverage, 0.0f, 1.0f);
            /// @note 中身は world→ノイズ UV スケールだが、Inspector では「まだら 1 周期の大きさ」で扱う。
            ImGui::DragFloat("Cloud Shadow Size", &sr.cloudShadowSize, 1.0f, 1.0f, 10000.0f, "%.0f m");
            ImGui::DragFloat("Cloud Shadow Speed", &sr.cloudShadowSpeed, 0.01f, 0.0f, 10.0f);
        });

    DrawComponentSection<scene::SunMoonRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sun Moon Renderer",
        [](scene::SunMoonRenderer& smr, EditorContext&) {
            ImGui::SeparatorText("Sun");
            ImGui::Checkbox("Sun Enabled", &smr.sunEnabled);
            if (smr.sunEnabled) {
                ImGui::DragFloat("Sun Disk Intensity", &smr.sunDiskIntensity, 0.1f, 0.0f, 1000.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("太陽ディスクの明るさ。空そのものの明るさは\n"
                                      "Sky Renderer の Sky Scatter Intensity です。");
            }

            ImGui::SeparatorText("Moon");
            ImGui::Checkbox("Moon Enabled", &smr.moonEnabled);
            if (smr.moonEnabled) {
                ImGui::DragFloat("Moon Size", &smr.moonSize, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Moon Brightness", &smr.moonBrightness, 0.01f, 0.0f, 10.0f);
                widgets::ColorEdit3("Moon Color", smr.moonColor);
            }
        });

    DrawComponentSection<scene::VolumetricCloudComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Volumetric Cloud",
        [](scene::VolumetricCloudComponent& cloud, EditorContext&) {
            /// @note DragFloat 直後に付ける説明。RangeField はウィジェット側が tooltip 引数を持つ。
            auto tip = [](const char* text) {
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
            };

            ImGui::SeparatorText("Layer");
            ImGui::DragFloat("Bottom Height", &cloud.bottomHeight, 5.0f, -10000.0f, 10000.0f);
            ImGui::DragFloat("Thickness", &cloud.thickness, 5.0f, 1.0f, 5000.0f);
            widgets::RangeField("Coverage", cloud.coverage, 0.0f, 1.0f);
            ImGui::DragFloat("Density", &cloud.density, 0.01f, 0.0f, 8.0f);

            ImGui::SeparatorText("Shape");
            /// @note world-to-noise の逆数をワールド寸法 [m] として編集する。
            ImGui::DragFloat("Cloud Size", &cloud.cloudSize, 5.0f, 50.0f, 100000.0f, "%.0f m");
            tip(
                "雲塊 1 周期の大きさ。大きいほど雄大な雲になります。\n"
                "Shape ノイズはタイラブルな 3D テクスチャなので、\n"
                "この距離ごとにまったく同じ雲が並びます。小さくすると\n"
                "雲は細かくなりますが、繰り返しが視界内で目立ちます。");
            ImGui::DragFloat("Weather Size", &cloud.weatherSize, 25.0f, 100.0f, 1000000.0f, "%.0f m");
            tip(
                "晴れ間と厚い塊が入れ替わる周期。Cloud Size の 5〜10 倍にしてください。\n"
                "Shape のタイル周期と十分ずらすことで、同じ雲が格子状に\n"
                "並んで見える繰り返しを視覚的に解きます。");
            widgets::RangeField("Weather Amount", cloud.weatherAmount, 0.0f, 1.0f, "%.2f",
                "雲量ムラの強さ。0 で空一面が同じ雲量になり (旧挙動)、\n"
                "タイリングがそのまま見えます。上げるほど晴れ間と\n"
                "厚い塊の差が大きくなります。");
            ImGui::DragFloat("Detail Size", &cloud.detailSize, 1.0f, 1.0f, cloud.cloudSize, "%.0f m");
            tip("雲の縁を侵食する細部ノイズの大きさ。Cloud Size より小さくします。");
            widgets::RangeField("Detail Strength", cloud.detailStrength, 0.0f, 1.0f, "%.2f",
                "高周波ノイズで雲の縁を侵食する量。上げるとちぎれた綿状になります。");
            widgets::RangeField("Bottom Softness", cloud.bottomSoftness, 0.01f, 0.9f, "%.2f",
                "雲底の丸み。小さいほど平らな底になります。");
            widgets::RangeField("Top Softness", cloud.topSoftness, 0.01f, 0.9f, "%.2f",
                "雲頂の散り方。大きいほど上へふわりと消えます。");
            ImGui::DragFloat("Evolution Speed", &cloud.evolutionSpeed, 0.05f, 0.0f, 20.0f);
            tip("流れとは別に、雲の形そのものが変化する速さ。0 で形が固定されます。");

            ImGui::SeparatorText("Wind");
            ImGui::DragFloat("Wind Speed", &cloud.windSpeed, 0.5f, 0.0f, 300.0f);
            DragVec2("Wind Direction", cloud.windDirection, 0.01f, -1.0f, 1.0f);

            ImGui::SeparatorText("Lighting");
            ImGui::DragFloat("Sun Intensity", &cloud.sunIntensity, 0.05f, 0.0f, 20.0f);
            tip(
                "太陽散乱の倍率。雲が暗くて見えないときはまずここを上げます。\n"
                "空の明るさ (Sky Renderer の Sky Day Brightness) に対する相対値です。");
            ImGui::DragFloat("Extinction", &cloud.extinction, 0.001f, 0.001f, 0.5f, "%.4f");
            tip(
                "密度 → 消散係数のスケール。上げるほど不透明で締まった雲、\n"
                "下げるほど薄く光を通す雲になります。Density と合わせて詰めます。");
            ImGui::DragFloat("Light Absorption", &cloud.lightAbsorption, 0.01f, 0.0f, 8.0f);
            tip("雲内部のセルフシャドウの濃さ。大きいほど陰影が強く暗い雲になります。");
            ImGui::DragFloat("Ambient Strength", &cloud.ambientStrength, 0.01f, 0.0f, 4.0f);
            tip("空から回り込む間接光の強さ。影側が黒く潰れるときに上げます。");
            widgets::RangeField("Ambient Gradient", cloud.ambientGradient, 0.0f, 1.0f, "%.2f",
                "雲底に届く間接光の割合。間接光は雲の天面から入るので、\n"
                "0 にすると底が真っ黒に、1 にすると上下均一になります。");
            widgets::RangeField("Multi Scatter", cloud.multiScatter, 0.0f, 1.0f, "%.2f",
                "多重散乱オクターブの寄与。0 だと単散乱のみで雲の内側が\n"
                "真っ黒に落ちます。上げるほど内部まで光が回ります。");
            widgets::RangeField("Powder Strength", cloud.powderStrength, 0.0f, 1.0f, "%.2f",
                "Beer-Powder。太陽側の縁を暗く落として立体感を出します。");
            widgets::RangeField("Anisotropy", cloud.anisotropy, 0.0f, 0.95f, "%.2f",
                "位相関数の前方散乱の鋭さ。上げるほど太陽の周りだけが強く光ります。");
            ImGui::DragFloat("Silver Lining", &cloud.silverLining, 0.01f, 0.0f, 4.0f);

            ImGui::SeparatorText("Color");
            widgets::ColorEdit3("Albedo", cloud.albedo);
            tip("雲そのものの色。ふつうは白のままにします。");
            widgets::ColorEdit3("Sun Tint", cloud.sunTint);
            tip("太陽に照らされた側へ掛ける色。夕焼けの雲を暖色へ寄せるときに使います。");
            widgets::ColorEdit3("Ambient Tint", cloud.ambientTint);
            tip("影側 (空からの間接光) へ掛ける色。空の色に合わせると雲が空になじみます。");

            ImGui::SeparatorText("Range");
            ImGui::DragFloat("Min Distance", &cloud.minDistance, 1.0f, 0.0f, cloud.maxDistance, "%.0f m");
            tip("この距離までは雲を出しません。");
            ImGui::DragFloat("Fade Distance", &cloud.fadeDistance, 1.0f, 1.0f, 5000.0f, "%.0f m");
            tip("Min Distance からこの距離をかけて雲を濃くしていきます。\n"
                "カメラが雲層の高さまで上がると視線が雲の内部から始まり、\n"
                "手前が濃すぎて画面が真っ白になります。ここを広げると\n"
                "雲の中を通り抜けられるようになります。");
            widgets::RangeField("Horizon Fade", cloud.horizonFade, 0.0f, 1.0f, "%.2f",
                "水平線ぎわのフェード幅 (Max Distance に対する割合)。\n"
                "視線を水平へ倒すと雲層に入る距離が伸び、Max Distance を\n"
                "越えた瞬間に雲が消えます。その直前まで不透明なので、\n"
                "地平線に硬い切れ目が出ます。ここを広げると溶けます。");
            ImGui::DragFloat("Max Distance", &cloud.maxDistance, 50.0f, 100.0f, 50000.0f, "%.0f m");

            ImGui::SeparatorText("Light Shafts");
            widgets::RangeField("Light Shaft Strength", cloud.lightShaftStrength, 0.0f, 1.0f, "%.2f",
                "雲の切れ間から差す光の線 (ゴッドレイ) の濃さ。\n"
                "体積光パスがこの雲の密度を直接引いて光芒を遮ります。\n"
                "Post Process Volume の Volumetric Light を有効にし、\n"
                "Max Distance を光芒を伸ばしたい距離まで上げてください。\n"
                "0 で遮蔽をやめ、光芒は一様な靄に戻ります。");

            ImGui::SeparatorText("Quality");
            ImGui::SliderInt("Step Count", &cloud.stepCount, 8, 96);
            ImGui::SliderInt("Light Step Count", &cloud.lightStepCount, 1, 8);
            tip("太陽方向セルフシャドウのステップ数。上げると陰影が正確になりますが重くなります。");
            ImGui::Checkbox("Half Resolution", &cloud.halfResolution);
            tip("半解像度でレイマーチしてから拡大合成します。\n"
                "描画ピクセルが 1/4 になる代わりに輪郭が甘くなります。");
        });

    DrawComponentSection<scene::DecalComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Decal",
        [](scene::DecalComponent& dc, EditorContext& ctx) {

            ImGui::SeparatorText("Material");
            widgets::AssetPathField("Material (.mat)", dc.materialPath, ".mat", ctx.projectRoot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("render_path = \"decal\" の .mat だけが使えます。\n"
                                  "空欄のときは下のテクスチャと色で組み込みシェーダーが描きます。");

            /// @note .mat 指定時に組込みの値は渡らないため、編集を無効にする。
            const bool usesMaterial = !dc.materialPath.empty();
            ImGui::BeginDisabled(usesMaterial);

            ImGui::SeparatorText("Textures");
            widgets::AssetPathField("Albedo (t0)", dc.albedoTexPath,
                                    widgets::kTextureAssetFilter, ctx.projectRoot);
            widgets::AssetPathField("Normal (t1)", dc.normalTexPath,
                                    widgets::kTextureAssetFilter, ctx.projectRoot);
            widgets::AssetPathField("Emissive (t3)", dc.emissiveTexPath,
                                    widgets::kTextureAssetFilter, ctx.projectRoot);

            ImGui::SeparatorText("Surface");
            ImGui::ColorEdit4("Albedo Color",     dc.albedoColor);
            widgets::RangeField("Normal Strength", dc.normalStrength, 0.0f, 2.0f);

            ImGui::EndDisabled();

            ImGui::SeparatorText("Angle Fade");
            /// @note 行に複数アイテムがあるため、hover 説明は RangeField の内部で扱う。
            widgets::RangeField("Angle Fade", dc.angleFadeStrength, 0.0f, 1.0f, "%.3f",
                "受け面が投影軸から傾くほどデカールを薄くします (0 で無効)。\n"
                "OBB 投影は斜めな面へ当てるとテクスチャが引き伸ばされ、長い筋になります。\n"
                "着弾痕や血痕が壁と床の角をまたいだ瞬間に「伸びた汚れ」として露見する、\n"
                "デカールで最も目立つ破綻がこれです。");
            widgets::RangeField("Fade Limit (deg)", dc.angleFadeDegrees, 0.0f, 89.0f,
                "%.0f\xc2\xb0", "これ以上寝た面ではデカールが完全に消えます。");

            ImGui::SeparatorText("Emissive");
            ImGui::ColorEdit3("Emissive Color", dc.emissiveColor);
            ImGui::DragFloat("Emissive Scale",  &dc.emissiveScale, 0.01f, 0.0f, 100.0f);

            ImGui::SeparatorText("Lifetime");
            ImGui::DragFloat("Lifetime (s)",  &dc.lifetime, 0.1f, -1.0f, 3600.0f, dc.lifetime < 0.0f ? "Permanent" : "%.1f s");
            ImGui::DragFloat("Fade In (s)",   &dc.fadeInTime, 0.05f, 0.0f, 60.0f,
                             dc.fadeInTime <= 0.0f ? "Instant" : "%.2f s");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("出現時に濃くなっていく時間。0 でいきなり全濃度 (従来の挙動)。\n"
                                  "血だまりが広がる・焦げが焼き付く、といった «痕が付く» 過程を作ります。\n"
                                  "Permanent なデカールでも効きます。");
            }
            if (dc.fadeInTime < 0.0f) dc.fadeInTime = 0.0f;
            ImGui::DragFloat("Fade Time (s)", &dc.fadeTime, 0.05f, 0.0f, 60.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("消える前のフェード時間。Lifetime が Permanent のときは効きません。");
            ImGui::BeginDisabled();
            ImGui::DragFloat("Age (s)", &dc.age, 0.0f, 0.0f, 0.0f, "%.2f s");
            /// @note VFX / script が指定する保存されないランタイム倍率。
            ImGui::DragFloat("Opacity", &dc.opacity, 0.0f, 0.0f, 0.0f, "%.2f");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Flipbook");
            ImGui::DragInt("Frame Count", &dc.frameCount, 1.0f, 0, 4096,
                           dc.frameCount > 1 ? "%d" : "Off");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("アトラスに詰めたコマ数。0 または 1 で無効 (UV は素通し)。\n"
                                  "焦げの広がり・血の乾き・亀裂の進行をテクスチャだけで作れます。\n"
                                  "コマの境界はバイリニアが隣を拾うので、各コマの周囲に余白を作ってください。");
            }
            if (dc.frameCount < 0) dc.frameCount = 0;
            if (dc.frameCount > 1) {
                ImGui::DragInt("Frames Per Row", &dc.framesPerRow, 1.0f, 1, dc.frameCount);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("横方向のコマ数。縦の段数は Frame Count から切り上げで決まります。");
                dc.framesPerRow = std::clamp(dc.framesPerRow, 1, dc.frameCount);

                ImGui::DragFloat("Frame Rate", &dc.frameRate, 0.5f, 0.0f, 240.0f,
                                 dc.frameRate > 0.0f ? "%.1f fps" : "Fit lifetime");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("0 なら Lifetime いっぱいで 1 周します (Permanent では進みません)。\n"
                                      ">0 なら秒あたりのコマ数。");
                }
                if (dc.frameRate < 0.0f) dc.frameRate = 0.0f;

                ImGui::Checkbox("Frame Loop", &dc.frameLoop);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("OFF: 最後のコマで止まる (焦げ・傷が «進んで残る»)\n"
                                      "ON : 先頭へ戻って繰り返す (電気の走る痕)");
                }
                if (dc.frameRate <= 0.0f && dc.lifetime < 0.0f) {
                    ImGui::TextColored({ 1.0f, 0.75f, 0.35f, 1.0f },
                        "Permanent + Frame Rate 0 では 1 コマ目のまま止まります");
                }
                const int rows = (dc.frameCount + dc.framesPerRow - 1) / dc.framesPerRow;
                ImGui::TextDisabled("アトラス %d x %d コマ", dc.framesPerRow, rows);
            }

            ImGui::SeparatorText("Sorting");
            ImGui::DragInt("Sort Order", &dc.sortOrder, 1.0f, -1024, 1024);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("小さいほど先に描く = 後ろになります。\n"
                                  "デカールは深度を書かないので、合成する順番だけが重なりの前後を決めます。\n"
                                  "同じ値のデカールは Hierarchy の並び順のままです。");
            }

            ImGui::SeparatorText("Receiver Layer Mask");
            /// @note ビット 0〜7 を個別チェックボックスで表示。残りは hex 入力で直接編集。
            static constexpr const char* kLayerNames[] = {
                "Default", "TransparentFX", "Ignore Raycast", "User Layer 3",
                "Water",   "UI",            "User Layer 6",   "User Layer 7"
            };
            for (int i = 0; i < 8; ++i) {
                bool checked = (dc.receiverLayerMask & (1u << i)) != 0;
                if (ImGui::Checkbox(kLayerNames[i], &checked)) {
                    if (checked) dc.receiverLayerMask |=  (1u << i);
                    else         dc.receiverLayerMask &= ~(1u << i);
                }
                if (i % 2 == 0) ImGui::SameLine(160.0f);
            }
            ImGui::InputScalar("Mask (hex)", ImGuiDataType_U32, &dc.receiverLayerMask,
                               nullptr, nullptr, "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal);
        });

    /// @name EnvironmentLightComponent
    DrawComponentSection<scene::EnvironmentLightComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Environment Light",
        [](scene::EnvironmentLightComponent& elc, EditorContext& ctx) {

            /// @note 有効化トグル — ヘッダーの enabled と連動するが、こちらが本体の操作点。
            ImGui::Checkbox("IBL Enabled", &elc.enabled);
            ImGui::Spacing();

            ImGui::SeparatorText("IBL Source");
            static constexpr const char* kSourceNames[] = { "Static DDS", "Dynamic Sky" };
            int source = static_cast<int>(elc.source);
            if (ImGui::Combo("Source", &source, kSourceNames, 2)) {
                source = (source < 0 || source > 1) ? 0 : source;
                elc.source = static_cast<scene::IblSource>(source);
            }

            if (elc.source == scene::IblSource::StaticDDS) {
                ImGui::SeparatorText("IBL Cubemaps (.dds)");
                widgets::AssetPathField("Irradiance##iblIrr", elc.irradiancePath, ".dds,.hdr", ctx.projectRoot);
                widgets::AssetPathField("Prefiltered##iblPre", elc.prefilterPath, ".dds,.hdr", ctx.projectRoot);
                widgets::AssetPathField("Raw HDR##iblRaw", elc.rawEnvironmentPath, ".dds", ctx.projectRoot);
                ImGui::DragFloat("Environment Rotation", &elc.rotationY, 0.5f, -360.0f, 360.0f, "%.1f deg");
            } else {
                ImGui::TextDisabled("SkyRenderer の空を実行時に IBL へ焼き込みます。");
            }

            ImGui::SeparatorText("Intensity");
            ImGui::DragFloat("Overall Intensity", &elc.intensity,    0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Diffuse Scale",     &elc.diffuseScale, 0.01f, 0.0f, 4.0f);
            ImGui::DragFloat("Specular Scale",    &elc.specularScale,0.01f, 0.0f, 4.0f);
            ImGui::SliderInt("Max Mip Level",     &elc.maxMipLevel,  1, 8, "%d (bake mips - 1)");

            if (elc.source == scene::IblSource::StaticDDS &&
                (elc.irradiancePath.empty() || elc.prefilterPath.empty()))
                ImGui::TextColored({1.0f, 0.7f, 0.2f, 1.0f},
                    "(!) パスが設定されていないと IBL は無効になります");
        });

    /// @name ReflectionProbeComponent
    DrawComponentSection<scene::ReflectionProbeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Reflection Probe",
        [](scene::ReflectionProbeComponent& rpc, EditorContext& ctx) {

            ImGui::SeparatorText("Cubemap (.dds)");
            widgets::AssetPathField("Cubemap Path", rpc.cubemapPath, ".dds,.hdr", ctx.projectRoot);

            ImGui::SeparatorText("Influence");
            ImGui::DragFloat("Intensity",        &rpc.intensity,       0.01f, 0.0f, 8.0f);
            ImGui::Checkbox("Box Influence",     &rpc.boxInfluence);
            if (rpc.boxInfluence) {
                float ext[3] = { rpc.boxExtents.x, rpc.boxExtents.y, rpc.boxExtents.z };
                if (widgets::DragAxes("Box Extents (m)", ext, 3, 0.05f, 0.0f, 1000.0f)) {
                    rpc.boxExtents = { ext[0], ext[1], ext[2] };
                }
            } else {
                ImGui::DragFloat("Influence Radius (m)", &rpc.influenceRadius, 0.1f, 0.0f, 500.0f);
            }

            ImGui::Spacing();
            ImGui::TextDisabled("(i) 局所反射ブレンドは将来の実装で有効になります");
        });

    /// @name LightProbeVolumeComponent
    DrawComponentSection<scene::LightProbeVolumeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Light Probe Volume",
        [](scene::LightProbeVolumeComponent& lpv, EditorContext&) {

            ImGui::SeparatorText("Volume");
            float ext[3] = { lpv.boxExtents.x, lpv.boxExtents.y, lpv.boxExtents.z };
            if (widgets::DragAxes("Box Extents (m)", ext, 3, 0.05f, 0.05f, 1000.0f))
                lpv.boxExtents = { ext[0], ext[1], ext[2] };
            int grid[3] = { lpv.probeCountX, lpv.probeCountY, lpv.probeCountZ };
            if (ImGui::DragInt3("Probe Count", grid, 0.1f, 1, 64)) {
                lpv.probeCountX = grid[0]; lpv.probeCountY = grid[1]; lpv.probeCountZ = grid[2];
            }
            const auto clamped = lpv.ClampedGrid();
            const math::Vector3 spacing{ lpv.boxExtents.x * 2.0f / static_cast<float>(clamped[0]),
                                         lpv.boxExtents.y * 2.0f / static_cast<float>(clamped[1]),
                                         lpv.boxExtents.z * 2.0f / static_cast<float>(clamped[2]) };
            ImGui::TextDisabled("%d probes / spacing %.2f x %.2f x %.2f m",
                                lpv.ProbeCount(), spacing.x, spacing.y, spacing.z);

            ImGui::SeparatorText("Lighting");
            ImGui::DragFloat("Intensity", &lpv.intensity, 0.01f, 0.0f, 8.0f);
            ImGui::DragFloat("Normal Bias (m)", &lpv.normalBias, 0.01f, 0.0f, 2.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("引く点を法線方向へずらす距離。壁の中に埋まったプローブの\n"
                                  "暗さが壁の表面へ滲むのを弱めます。間隔の 1/4 前後が目安。");
            ImGui::DragFloat("Edge Fade (m)", &lpv.edgeFade, 0.05f, 0.0f, 50.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("箱の縁でこの幅だけ使って外側 (外のボリューム / IBL の環境光) へ戻します。\n"
                                  "0 で境目が段になります。");
            ImGui::SliderFloat("Specular Occlusion", &lpv.specularOcclusion, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("プローブで分かった暗さを鏡面反射にも掛けます。\n"
                                  "屋内に残る空の青い映り込みを消します。");

            ImGui::SeparatorText("Bake");
            ImGui::SliderInt("Bounces", &lpv.bounces, 1, 4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("1 = 直接光の 1 回反射。2 以上は前回の結果を受けた面を描き直すので、\n"
                                  "部屋の奥まで光が回ります (焼く時間は回数倍)。");
            ImGui::SliderInt("Capture Resolution", &lpv.captureResolution, 8, 128, "%d px");
            ImGui::SliderInt("Probes Per Frame", &lpv.probesPerFrame, 1, 64);
            ImGui::SliderFloat("Deringing", &lpv.deringing, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("強い日だまりの反対側に出る暗い輪 (SH のリンギング) を弱めます。\n"
                                  "上げるほど光の向きがぼやけます。");
            ImGui::Checkbox("Reject Inside Geometry", &lpv.rejectInsideGeometry);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("壁や床の中に埋まったプローブを捨て、周りのプローブで埋めます。\n"
                                  "壁際の黒いにじみを防ぎます (焼く時間は約 2 倍)。");
            ImGui::Checkbox("Realtime Update", &lpv.realtimeUpdate);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("焼き終えたら最初から焼き直し続けます。昼夜や動く光源に追従しますが、\n"
                                  "毎フレーム Probes Per Frame x 6 面ぶんのシーン描画がかかります。");

            /// @note 進捗は «何回目の焼きの何個目か»。ランタイム値なので保存されず、Play の往復で最初から焼き直す。
            const int total = lpv.ProbeCount();
            const int passes = std::clamp(lpv.bounces, 1, 8);
            if (lpv.runtimeBaking) {
                const int pass = std::min(lpv.runtimePass, passes - 1);
                const float progress = static_cast<float>(pass * total + lpv.runtimeCursor)
                                     / static_cast<float>(std::max(passes * total, 1));
                char label[64];
                std::snprintf(label, sizeof(label), "bounce %d/%d  %d/%d", pass + 1, passes, lpv.runtimeCursor, total);
                ImGui::ProgressBar(lpv.realtimeUpdate && lpv.runtimeReady ? 1.0f : progress, ImVec2(-1.0f, 0.0f), label);
            } else if (lpv.runtimeReady) {
                ImGui::TextColored({ 0.5f, 0.9f, 0.5f, 1.0f }, "Baked (%d probes, %d bounce%s)", total, passes, passes > 1 ? "s" : "");
            } else {
                ImGui::TextDisabled("未ベイク — IBL (Environment Light) が有効なときに自動で焼きます");
            }
            if (ImGui::Button("Bake", ImVec2(-1.0f, 0.0f)))
                lpv.bakeRequested = true;
        });

    /// @name AtmosphericScatteringComponent
    DrawComponentSection<scene::AtmosphericScatteringComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Atmospheric Scattering",
        [](scene::AtmosphericScatteringComponent& atm, EditorContext&) {

            ImGui::SeparatorText("Fog");
            ImGui::Checkbox("Fog Enabled", &atm.fogEnabled);
            if (atm.fogEnabled) {
                static constexpr const char* kFogSourceNames[] = { "Exponential", "Atmosphere" };
                int fogSource = static_cast<int>(atm.fogSource);
                if (ImGui::Combo("Fog Source", &fogSource, kFogSourceNames, 2)) {
                    fogSource = (fogSource < 0 || fogSource > 1) ? 0 : fogSource;
                    atm.fogSource = static_cast<scene::FogSource>(fogSource);
                }
                ImGui::DragFloat("Density",        &atm.fogDensity, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Far Distance (m)",&atm.fogFar,    1.0f,   0.0f, 10000.0f);
                if (atm.fogSource == scene::FogSource::Exponential) {
                    widgets::ColorEdit3("Fog Color", atm.fogColor);
                } else {
                    ImGui::TextDisabled("Fog color は SkyRenderer の大気散乱から計算されます。");
                }
            }
        });

    /// @name PostProcessVolumeComponent
    DrawComponentSection<scene::PostProcessVolumeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Post Process Volume",
        [](scene::PostProcessVolumeComponent& ppv, EditorContext& ctx) {

            ImGui::SeparatorText("Profile");

            /// @note 未割当の volume は何も適用しない。設定の所有者は共有 .fzdata。
            widgets::AssetPathField("Profile", ppv.profile.ref.path, ".fzdata", ctx.projectRoot);
            widgets::AcceptAssetPathDrop(ppv.profile.ref.path, ".fzdata");

            const auto* resolvedProfile = ppv.profile.Get();
            const bool usingProfile = resolvedProfile != nullptr;
            if (ppv.profile.ref.path.empty()) {
                ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f },
                    "Post Process Profile (.fzdata) が未設定です — このボリュームは何も適用しません");
            } else if (!usingProfile) {
                ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f },
                    "プロファイルを解決できません — パス切れか型が不一致です");
            }

            /// @note プロファイルに定義された効果だけがこの volume の対象になる。
            if (usingProfile) {
                std::string driven;
                int inactiveCount = 0;
                for (const auto& entry : resolvedProfile->overrides) {
                    if (!entry) continue;
                    if (!entry->active) { ++inactiveCount; continue; }
                    if (!driven.empty()) driven += ", ";
                    driven += entry->GetDisplayName();
                }
                if (driven.empty()) {
                    ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f },
                        "このプロファイルは何も上書きしません (Override が 0 個)");
                } else {
                    ImGui::TextDisabled("上書きする効果: %s", driven.c_str());
                    if (inactiveCount > 0)
                        ImGui::TextDisabled("(ほかに %d 個が一時無効)", inactiveCount);
                }
            }

            ImGui::SeparatorText("Volume");
            ImGui::Checkbox("Global",          &ppv.isGlobal);
            ImGui::DragInt("Priority",         &ppv.priority);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("小さいものから順に合成する。大きいほど後勝ちで強く出る。");
            ImGui::DragFloat("Blend Weight",   &ppv.blendWeight,     0.01f, 0.0f, 1.0f);
            if (!ppv.isGlobal) {
                ImGui::DragFloat("Influence Radius (m)", &ppv.influenceRadius, 0.5f, 0.1f, 1000.0f);
                ImGui::DragFloat("Blend Distance (m)",   &ppv.blendDistance,   0.1f, 0.0f, 1000.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("境界の内側でこの距離だけかけてフェードする。\n"
                                      "0 にすると半径をまたいだ瞬間にルックが飛ぶ。");
            }

            /// @note プロファイル編集は全参照へ届くため、volume 固有の編集欄として表示しない。
            if (usingProfile) {
                ImGui::Spacing();
                ImGui::TextDisabled("ルックの編集は Project ウィンドウで .fzdata を選択して行います。");
            }
        });

}


} // namespace fbzz::editor
