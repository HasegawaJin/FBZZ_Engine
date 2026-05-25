// FBZZ Engine
// RenderSettings.hpp | fbzz::renderer
// レンダリング設定とポストプロセスパラメーター
// RenderSystem に渡すフレーム単位の設定値をまとめる。
// 機能フラグと調整値を分け、UI から変更しやすくする。
#pragma once

namespace fbzz::renderer {

struct RenderSettings {
    // レンダーパス on/off
    bool wireframeMode = false;
    bool shadowEnabled = true;
    bool bloomEnabled  = true;
    bool fxaaEnabled   = true;
    bool fogEnabled    = true;
    bool showColliders = false;

    // ポストプロセスパラメーター
    float exposure   = 1.0f;
    float fogDensity = 0.06f;
    float fogFar     = 10.0f;
    float fogColor[3] = { 0.01f, 0.01f, 0.04f };
};

} // namespace fbzz::renderer
