// FBZZ Engine
// IShader.hpp | fbzz::renderer
// Renderer shader interface
#pragma once
#include <string>

namespace fbzz::renderer 
{

    class IShader 
    {
    public:
        virtual ~IShader() = default;

        // シェーダーのメタ情報
        virtual const std::string& GetPath() const = 0;
    };

} // namespace fbzz::renderer
