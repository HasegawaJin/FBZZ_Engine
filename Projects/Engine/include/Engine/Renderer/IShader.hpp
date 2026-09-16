/// @file    IShader.hpp
/// @brief   Shader の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// HLSL コンパイル済みオブジェクトなどの具体表現を隠す。
/// Renderer は DrawCall のシェーダーハンドルを ResourceManager で解決して使う。
#pragma once
#include "ShaderDescriptor.hpp"
#include <string>

namespace fbzz::renderer
{

    class IShader
    {
    public:
        virtual ~IShader() = default;

        virtual const std::string&      GetPath()       const = 0;
        virtual const ShaderDescriptor& GetDescriptor() const = 0;
    };

} // namespace fbzz::renderer
