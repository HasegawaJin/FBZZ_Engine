// FBZZ Engine
// RenderSettings.hpp | fbzz::renderer
// レンダリング設定とポストプロセスパラメーター
// RenderSystem に渡すフレーム単位の設定値をまとめる。
// 機能フラグと調整値を分け、UI から変更しやすくする。
#pragma once
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

struct RenderSelectionID {
    uint32_t index = 0xFFFFFFFFu;
    uint32_t generation = 0;
};

struct RenderSettings {
    // レンダーパス on/off
    bool wireframeMode = false;
    bool shadowEnabled = true;
    bool bloomEnabled  = true;
    bool fxaaEnabled   = true;
    bool fogEnabled    = true;
    bool showColliders = false;
    bool showSelectionOutline = true;

    // ポストプロセスパラメーター
    float exposure   = 1.0f;
    float fogDensity = 0.06f;
    float fogFar     = 10.0f;
    float fogColor[3] = { 0.01f, 0.01f, 0.04f };
    float outlineWidth = 0.045f;
    float outlineColor[4] = { 1.0f, 0.82f, 0.22f, 1.0f };
    std::vector<RenderSelectionID> selectedObjects;
};

} // namespace fbzz::renderer
