/// @file    FiberComponent.hpp
/// @brief   静的表面へ毛皮・芝を追加する描画方式と共有マテリアル参照。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Renderer/FiberMotionHistory.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace fbzz::scene {

enum class FiberRenderMode { SHELL, FIN, HYBRID, BLADE };

/// @note 同じ GameObject の MeshRenderer が根元の表面を持つ。GPU 資源は保存しない。
/// @see Docs/design/fiber-rendering.md
/// @see https://hhoppe.com/fur.pdf
struct FiberComponent {
    bool m_enabled = true;
    FiberRenderMode m_mode = FiberRenderMode::SHELL;
    /// @note 既定は 16 層・距離 LOD 有効。24 層は GBuffer だけで Player 1 体に約 3.5 ms かかった (fiber-rendering.md «負荷と影»)。
    int m_shellCount = 16;
    bool m_distanceLod = true;
    int m_minShellCount = 8;
    float m_lodNear = 8.0f;
    float m_lodFar = 40.0f;
    float m_bladeDensity = 400.0f;
    float m_bladeWidth = 0.015f;
    int m_seed = 1;
    int m_terrainPatchCells = 2;
    /// @note 地形の層番号。-1 は全面、0 以上はその層の重みで葉を間引く。
    /// @see Docs/design/terrain-layers.md §5 Fiber を層で制御する
    int m_terrainLayer = -1;
    /// @note 4 隅の層の重みの最大値がこれ未満のセルはパッチから省く [0, 1]。
    float m_terrainLayerThreshold = 0.25f;
    /// @note 受け取る FlowField のチャンネル (ビットマスク)。-1 は全チャンネル、0 は局所の場を受けない。環境風は常に受ける。
    int m_flowChannels = -1;
    /// @note 影だけの層数の上限。影は層の隙間が見えにくく、Shell の負荷の大半を占める (カスケード・光源の面ごとに全層を描く)。
    int m_shadowShellCount = 6;
    /// @name 個体ごとの上書き。共有 .mat の値へ掛ける。スクリプトから書き換えてよい
    /// @{
    /// @note 根元色・毛先色の RGB に掛ける (線形)。
    math::Vector3 m_colorTint{ 1.0f, 1.0f, 1.0f };
    float m_lengthScale = 1.0f;
    float m_densityScale = 1.0f;
    float m_clumpingScale = 1.0f;
    /// @note 空なら .mat の fiberMask (tex5)。入れるとこの個体だけ差し替える。
    std::string m_maskPath;
    /// @}
    /// @note 反射・保存の対象外。Play 復元時に古いビューの変形履歴を持ち越さない。
    std::vector<renderer::FiberMotionHistory> m_motionHistories;
    std::string m_materialPath = "guid:0c678cd29e1841f1b98bd553e2d8c013|Assets/Materials/Fiber/Fur.mat";

    const char* GetTypeName() const { return "Fiber"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", m_enabled);
        const char* labels[] = { "Shell", "Fin", "Hybrid", "Blade" };
        int mode = static_cast<int>(m_mode);
        r.Enum("mode", mode, labels);
        m_mode = static_cast<FiberRenderMode>(std::clamp(mode, 0, 3));
        r.IntRange("shellCount", m_shellCount, 1, 64);
        r.Tooltip("Shell の層数。Fin 単独では使用しません。");
        r.Field("distanceLod", m_distanceLod);
        r.Tooltip("lodNear〜lodFar で層数を minShellCount まで減らし、lodFar より遠くは描きません。");
        r.IntRange("minShellCount", m_minShellCount, 1, 64);
        r.FloatRange("lodNear", m_lodNear, 0.0f, 1000.0f);
        r.FloatRange("lodFar", m_lodFar, 0.1f, 2000.0f);
        r.FloatRange("bladeDensity", m_bladeDensity, 1.0f, 2000.0f);
        r.Tooltip("Blade の本数 / ローカル面積。近距離で途切れない連続した葉を配置します。");
        r.FloatRange("bladeWidth", m_bladeWidth, 0.001f, 0.5f);
        r.IntRange("seed", m_seed, 0, 65535);
        r.IntRange("terrainPatchCells", m_terrainPatchCells, 1, 32);
        r.IntRange("terrainLayer", m_terrainLayer, -1, 254);
        r.Tooltip("Terrain の層番号。-1 = 地形全体。0 以上ならその層の重みで生やします。");
        r.FloatRange("terrainLayerThreshold", m_terrainLayerThreshold, 0.0f, 1.0f);
        r.Tooltip("セルの 4 隅の層の重みの最大値がこの値未満なら、そのセルには生やしません。");
        r.Field("flowChannels", m_flowChannels);
        r.Tooltip("受け取る Flow Field のチャンネル (ビットマスク)。-1 で全チャンネル、0 で局所の場を無視します。環境風は常に受けます。");
        r.FileField("materialPath", m_materialPath, ".mat");
        r.Tooltip("Fiber マテリアル。Mesh Renderer / Skinned Mesh Renderer / Terrain と併用します。");
        r.IntRange("shadowShellCount", m_shadowShellCount, 1, 64);
        r.Tooltip("影に描く Shell の層数の上限。影は層の隙間が目立たず、層を減らすと負荷が大きく下がります。");
        r.Field("colorTint", m_colorTint);
        r.Tooltip("この個体だけ根元色・毛先色に掛ける色 (1 で .mat のまま)。");
        r.FloatRange("lengthScale", m_lengthScale, 0.0f, 4.0f);
        r.FloatRange("densityScale", m_densityScale, 0.0f, 4.0f);
        r.FloatRange("clumpingScale", m_clumpingScale, 0.0f, 4.0f);
        r.Tooltip("長さ・密度・毛束を .mat の値に掛けます。密度と毛束は掛けた結果が 1 で頭打ちです。");
        r.FileField("maskPath", m_maskPath, ".png");
        r.Tooltip("この個体だけ fiberMask を差し替えます (R=密度 G=長さ B=毛束 A=毛先色)。空なら .mat の tex5。");
    }
};

} // namespace fbzz::scene
