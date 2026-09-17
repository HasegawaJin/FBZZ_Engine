/// @file    DX12Shader.hpp
/// @brief   DXBC / DXIL シェーダーバイトコードと入力レイアウト情報の DirectX 12 表現。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Engine/Renderer/IShader.hpp>
#include <d3d12.h>
#include <string>
#include <vector>

namespace fbzz::renderer {

class DX12Shader final : public IShader {
public:
    bool Init(const std::string& path);
    const std::string& GetPath() const override { return m_path; }
    const ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
    D3D12_SHADER_BYTECODE GetVertexBytecode() const;
    D3D12_SHADER_BYTECODE GetPixelBytecode() const;
    D3D12_SHADER_BYTECODE GetComputeBytecode() const;
    bool IsCompute() const { return !m_computeBlob.empty(); }
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& GetInputLayout() const { return m_inputElements; }
    /// リフレクションで密詰め (tightly packed) を仮定して算出した頂点ストライド。
    /// 実際の頂点バッファのストライドと一致しない場合、入力レイアウトのオフセットがずれて
    /// ジオメトリが壊れる (描画ゼロピクセル化) ため、Submit 側で照合診断に使う。
    uint32_t GetReflectedStride() const { return m_reflectedStride; }

private:
    static std::string CompiledBase(const std::string& path);
    static std::vector<uint8_t> LoadBinary(const std::string& path);
    /// PS バイトコード (DXBC / DXIL) から定数とテクスチャバインドをリフレクションする。
    static ShaderDescriptor BuildDescriptor(const std::vector<uint8_t>& psBlob);
    bool ReflectVertexInput();

    std::string m_path;
    ShaderDescriptor m_descriptor;
    std::vector<uint8_t> m_vertexBlob;
    std::vector<uint8_t> m_pixelBlob;
    std::vector<uint8_t> m_computeBlob;
    std::vector<std::string> m_semanticNames;
    std::vector<D3D12_INPUT_ELEMENT_DESC> m_inputElements;
    uint32_t m_reflectedStride = 0;
};

} // namespace fbzz::renderer
