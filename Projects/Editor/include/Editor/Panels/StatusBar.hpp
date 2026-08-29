/// @file    StatusBar.hpp
/// @brief   画面下端に固定描画される情報バーとドロワーパネルの開閉操作。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <string>

namespace fbzz::editor { struct EditorContext; }

namespace fbzz::editor {

// 下端ドロワーの開閉ボタン。開いている間は押下色になる。
// visible が nullptr なら何も描かない。描画後は区切りを入れて次の項目へ続く。
// WHY: メイン Editor と VFX Editor で同じ操作・同じ見た目にするため、実装を1か所に置く。
//      別々に書くと片方だけラベルや押下表現が変わり、同じ機能に見えなくなる。
void DrawDrawerToggle(const char* label, bool* visible, const char* tooltipTarget);

class StatusBar {
public:
    // 画面下端の状態表示と、下端ドロワー (Asset Browser / Console) の開閉操作を描画する。
    // 各 visible ポインタが nullptr のトグルは描画しない。
    // WHY: どちらも「一時的に開いて確認し、すぐ畳む」使い方をするパネルで、
    //      View メニューやドックタブを探すより下端の定位置にある方が速い。
    void Draw(EditorContext& ctx, bool* assetBrowserVisible = nullptr,
              bool* consoleVisible = nullptr);
    void SetMessage(const std::string& msg) { m_message = msg; }

private:
    std::string m_message;
    float       m_fps       = 0.0f;
    float       m_fpsTimer  = 0.0f;
    int         m_fpsCount  = 0;
};

} // namespace fbzz::editor
