// FBZZ Engine
// IShader.hpp | fbzz::renderer
// Shader の抽象インターフェース
// HLSL コンパイル済みオブジェクトなどの具体表現を隠す。
// Renderer は DrawCall のシェーダーハンドルを ResourceManager で解決して使う。
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
