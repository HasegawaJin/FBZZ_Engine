/// @file    FzVertexCompat.hpp
/// @brief   旧版バイナリの静的頂点を現行 renderer::Vertex へ読み広げる補助
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Renderer/Mesh.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::asset {

/// 静的頂点配列を読む。頂点カラーを持たない旧版は白で埋めて広げる。
/// @param hasVertexColor ファイルのバージョンが頂点カラーを含むか。
inline bool ReadStaticVertices(BinaryReader& reader,
                               uint32_t count,
                               bool hasVertexColor,
                               std::vector<renderer::Vertex>& out)
{
    out.resize(count);
    if (count == 0) return true;

    if (hasVertexColor)
        return reader.ReadBytes(out.data(), static_cast<size_t>(count) * sizeof(renderer::Vertex));

    std::vector<FzVertexV1> legacy(count);
    if (!reader.ReadBytes(legacy.data(), static_cast<size_t>(count) * sizeof(FzVertexV1)))
        return false;

    for (uint32_t i = 0; i < count; ++i) {
        const FzVertexV1& src = legacy[i];
        renderer::Vertex& dst = out[i];
        dst.position = { src.position[0], src.position[1], src.position[2] };
        dst.normal   = { src.normal[0],   src.normal[1],   src.normal[2]   };
        dst.tangent  = { src.tangent[0],  src.tangent[1],  src.tangent[2]  };
        dst.uv       = { src.uv[0], src.uv[1] };
        dst.color    = { 1.0f, 1.0f, 1.0f, 1.0f };
    }
    return true;
}

} // namespace fbzz::asset
