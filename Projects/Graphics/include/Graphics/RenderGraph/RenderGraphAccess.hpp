/// @file    RenderGraphAccess.hpp
/// @brief   描画パスが申告する資源アクセスの軽量な宣言型。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <string>

namespace fbzz::renderer {

enum class RenderResourceUsage {
    Read,
    Write,
    ReadWrite
};

enum class RenderResourceAccessPurpose {
    UNSPECIFIED,
    BUILD_INPUT,
    AS_WRITE,
    TRACE_READ,
    SHADER_READ,
    UAV
};

struct RenderResourceAccess {
    std::string name;
    RenderResourceUsage usage = RenderResourceUsage::Read;
    RenderResourceAccessPurpose purpose = RenderResourceAccessPurpose::UNSPECIFIED;
};

} /// @note namespace fbzz::renderer
