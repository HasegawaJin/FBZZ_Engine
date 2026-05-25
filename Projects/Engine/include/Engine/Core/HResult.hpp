// FBZZ Engine
// HResult.hpp | fbzz::core
// DirectX 系 HRESULT の検査マクロ
// 回復不能な DX 呼び出し失敗を assert で止めるための薄い補助。
// エンジン方針どおり例外には変換しない。
#pragma once
#include "Logger.hpp"

#define FBZZ_HR_CHECK(hr)                                               \
    do {                                                                 \
        if (FAILED(hr)) {                                                \
            FBZZ_LOG_ERROR("HRESULT 失敗: 0x%08X", (unsigned)(hr));     \
            __debugbreak();                                              \
            return false;                                                \
        }                                                                \
    } while (0)
