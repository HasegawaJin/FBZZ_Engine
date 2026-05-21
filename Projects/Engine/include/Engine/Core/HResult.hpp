// FBZZ Engine
// HResult.hpp | fbzz::core
// HRESULT チェックマクロ
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
