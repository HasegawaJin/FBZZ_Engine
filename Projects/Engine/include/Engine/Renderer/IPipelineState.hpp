// FBZZ Engine
// IPipelineState.hpp | fbzz::renderer
// Renderer pipeline state interface
#pragma once
#include "RenderState.hpp"

namespace fbzz::renderer
{
    class IPipelineState 
    {
    public:
        virtual ~IPipelineState() = default;

        virtual const PipelineStateDesc& GetDesc() const = 0;
    };
}
