/// @file    RenderBindingGuard.cpp
/// @brief   束縛毒スイッチの有効判定。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <Engine/Renderer/RenderBindingGuard.hpp>

#include <Engine/Core/Logger.hpp>

#include <Windows.h>

namespace fbzz::renderer::bindingguard {

bool IsEnabled()
{
    if constexpr (!kBuiltIn) {
        return false;
    } else {
        /// @note 起動中に変わるものではないので 1 回だけ読む。
        static const bool enabled = [] {
            wchar_t     value[8]{};
            const DWORD length = GetEnvironmentVariableW(L"FBZZ_RENDER_BINDING_GUARD", value, 8);
            const bool  on     = (length > 0 && length < 8) && value[0] != L'0';

            if (on) {
                FBZZ_LOG_WARN("RenderBindingGuard: パス境界で RT 束縛を無効化します。"
                              "自分で SetRenderTarget を呼ばないパスの描画は画面から消えます "
                              "(切るには FBZZ_RENDER_BINDING_GUARD=0)");
            }
            return on;
        }();
        return enabled;
    }
}

} // namespace fbzz::renderer::bindingguard
