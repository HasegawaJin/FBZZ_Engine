/// @file    FiberComponent.hpp
/// @brief   静的表面へ毛皮・芝を追加する描画方式と共有マテリアル参照。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Renderer/FiberMotionHistory.hpp>
#include <algorithm>
#include <vector>

namespace fbzz::scene {

enum class FiberRenderMode { SHELL, FIN, HYBRID, BLADE };

/// @note 同じ GameObject の MeshRenderer が根元の表面を持つ。GPU 資源は保存しない。
/// @see Docs/design/fiber-rendering.md
/// @see https://hhoppe.com/fur.pdf
struct FiberComponent {
    bool m_enabled = true;
    FiberRenderMode m_mode = FiberRenderMode::SHELL;
    int m_shellCount = 24;
    bool m_distanceLod = false;
    int m_minShellCount = 8;
    float m_lodNear = 8.0f;
    float m_lodFar = 40.0f;
    float m_bladeDensity = 400.0f;
    float m_bladeWidth = 0.015f;
    int m_seed = 1;
    int m_terrainPatchCells = 2;
    /// @note 受け取る FlowField のチャンネル (ビットマスク)。-1 は全チャンネル、0 は局所の場を受けない。環境風は常に受ける。
    int m_flowChannels = -1;
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
        r.IntRange("minShellCount", m_minShellCount, 1, 64);
        r.FloatRange("lodNear", m_lodNear, 0.0f, 1000.0f);
        r.FloatRange("lodFar", m_lodFar, 0.1f, 2000.0f);
        r.FloatRange("bladeDensity", m_bladeDensity, 1.0f, 2000.0f);
        r.Tooltip("Blade の本数 / ローカル面積。近距離で途切れない連続した葉を配置します。");
        r.FloatRange("bladeWidth", m_bladeWidth, 0.001f, 0.5f);
        r.IntRange("seed", m_seed, 0, 65535);
        r.IntRange("terrainPatchCells", m_terrainPatchCells, 1, 32);
        r.Field("flowChannels", m_flowChannels);
        r.Tooltip("受け取る Flow Field のチャンネル (ビットマスク)。-1 で全チャンネル、0 で局所の場を無視します。環境風は常に受けます。");
        r.FileField("materialPath", m_materialPath, ".mat");
        r.Tooltip("Fiber マテリアル。Mesh Renderer / Skinned Mesh Renderer / Terrain と併用します。");
    }
};

} // namespace fbzz::scene
