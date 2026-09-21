/// @file    DX12HdriBaker.hpp
/// @brief   IIblBaker の DirectX 12 実装 — Editor 専用の HDRI (.hdr / .exr) → DDS ベイカー。
/// @author  Hasegawa Jin
/// @date    2026-07-15
/// @note 役割の切り分け:
/// @note - DX12HdriBaker (このファイル): Editor から呼ばれ、equirect float ピクセルを
/// @note 4 枚の DDS (env / irradiance / prefilter / brdf) + .ibl 記述子へ焼き出す。
/// @note DX11IblBaker::Bake() の DX12 版。IRenderer::CreateIblBaker() が生成する。
/// @note - DX12IblBaker (別ファイル): BakeSkyLight() の実行時畳み込み経路 (空連動 IBL)。
/// @note こちらは開いているフレームのコマンドリストへ記録する GPU 常駐テクスチャ生成。
/// @note 設計上の注意 (DX11IblBaker との差分):
/// @note - DX11 は即時コンテキストで完結するが、DX12 は「記録 → submit → フェンス待ち」が必要。
/// @note そのため本クラスは **フレームサイクルから独立した自前のコマンドリスト・フェンス**を持ち、
/// @note 各フェーズを同期実行する (DX11 の即時実行と同じ意味論を再現。一度きりの Editor 操作なので
/// @note GPU 完全待機のストールは許容する)。
/// @note - DX12 には ID3D11DeviceContext::GenerateMips が無い。env cubemap の mip 連鎖は
/// @note GPU で mip0 を焼いた後、DirectXTex (CPU) で生成し直して GPU へ再アップロードする。
/// @note これにより prefilter の PDF ベース LOD 参照 (firefly 低減) が DX11 と一致する。
/// @note - Compute の Root Signature / static sampler / 定数スロット契約 (b0 / t0 / u0) は
/// @note DX12PsoCache の汎用 Compute Root Signature をそのまま共有する。
/// @note 依存方向: 上位レイヤーからは IIblBaker* としてのみ触れられる (DX12 型は漏らさない)。
#pragma once

#include <Graphics/Renderer/IIblBaker.hpp>
/// @note INVALID_BINDLESS_INDEX 用。
#include <Graphics/Renderer/ITexture.hpp>

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

class DX12Context;
class DX12PsoCache;

class DX12HdriBaker final : public IIblBaker {
public:
    /// @note context / psoCache は DX12Renderer が所有する。本クラスは Editor のベイク中のみ生存する
    /// @note 一時オブジェクトであり、両者の寿命内で使われる (CreateIblBaker() の呼び出し規約)。
    DX12HdriBaker(DX12Context* context, DX12PsoCache* psoCache);
    ~DX12HdriBaker() override;

    /// @note HDRI → DDS × 4 + .ibl。失敗時は false (例外は投げない)。
    [[nodiscard]] bool Bake(
        const IblBakeInput& input,
        const std::string&  outputDir,
        const std::string&  baseName,
        IblBakeOutput&      output) override;

private:
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;

    /// @note shader-visible ヒープから 1 スロット確保して CPU/GPU ハンドルと添字を返す。
    struct Descriptor {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        /// @note 自前ヒープ内の添字。
        /// @note これがそのまま bindless の添字になる理由: ResourceDescriptorHeap[] は
        /// @note       «そのとき束縛されているヒープ» を引く。このベイカーは自分のヒープを
        /// @note       SetDescriptorHeaps しているので、共有ヒープの永続レンジへ複製する必要がない。
        uint32_t index = INVALID_BINDLESS_INDEX;
    };

    /// @note 一度だけ構築する device レベルのリソース (コマンドリスト・フェンス・ヒープ・CB アップロード)。
    bool EnsureCommon();
    /// @note input.compiledShadersDir が変わった場合のみ 4 つの Compute PSO を再ロードする。
    bool EnsurePipelines(const std::string& compiledShadersDir);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LoadComputePso(const std::string& csoPath);

    /// @note フェーズ境界。BeginRecording でアロケーターとリング (ヒープ / CB) をリセットし、
    /// @note ExecuteAndWait で submit → フェンス完全待機する。
    void BeginRecording();
    bool ExecuteAndWait();

    /// @note shader-visible CBV/SRV/UAV ヒープからの線形割当。
    Descriptor AllocateDescriptor();
    /// @note CB アップロードリングへ 256B アライン格納し、root CBV 用の GPU VA を返す (0 なら失敗)。
    D3D12_GPU_VIRTUAL_ADDRESS PushConstants(const void* data, size_t size);

    /// @note GPU リソース生成 (すべて committed / DEFAULT ヒープ)。
    Resource CreateCubemap(uint32_t size, uint32_t mipCount);   ///< @note R16G16B16A16F, 6 面, UAV 可, 初期状態 UAV
    Resource CreateLut(uint32_t size);                          ///< @note R16G16B16A16F, 2D, UAV 可, 初期状態 UAV

    /// @note ビュー作成 (割当済みスロットの CPU ハンドルへ) して table バインド用 GPU ハンドルを返す。
    uint32_t CreateEquirectSrv(ID3D12Resource* equirect);
    uint32_t CreateCubeSrv(ID3D12Resource* cube, uint32_t mipCount);
    uint32_t CreateFaceUav(ID3D12Resource* cube, uint32_t face, uint32_t mip);
    uint32_t CreateLutUav(ID3D12Resource* lut);

    /// @note subresources を一時 upload バッファ経由で dest へコピー記録する (footprint / row-pitch 対応)。
    /// @note uploadKeepAlive は GPU 完了 (ExecuteAndWait) まで生存させる必要がある。
    bool RecordUpload(ID3D12Resource* dest, const D3D12_SUBRESOURCE_DATA* subs,
                      uint32_t count, Resource& uploadKeepAlive);
    /// @note equirect float* を R32G32B32A32F の 2D テクスチャへアップロードし NON_PIXEL_SHADER_RESOURCE へ遷移。
    bool UploadEquirect(const float* pixels, uint32_t width, uint32_t height,
                        Resource& out, Resource& uploadKeepAlive);

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after);
    /// @brief CS を 1 回記録する。
    /// @param srvIndex SRV の bindless 添字 (t0 相当)。SRV を読まない CS は INVALID_BINDLESS_INDEX。
    /// @param uavIndex UAV の bindless 添字 (u0 相当)。
    /// @note 添字は «このベイカーが束縛している自前ヒープ» 内の位置。
    /// @note       ResourceDescriptorHeap[] は束縛中のヒープを引くので、共有ヒープへ複製する必要がない。
    /// @see  Docs/design/bindless.md
    void Dispatch(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb,
                  uint32_t srvIndex, uint32_t uavIndex, uint32_t size);

    /// @note GPU テクスチャを CaptureTexture (DirectXTex DX12) で読み戻して DDS 保存する。
    bool SaveDds(ID3D12Resource* resource, bool isCubeMap, const std::string& absPath);

    static std::vector<uint8_t> LoadBinary(const std::string& path);

    DX12Context*  m_context  = nullptr;
    DX12PsoCache* m_psoCache = nullptr;
    ID3D12Device* m_device   = nullptr;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
    HANDLE   m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    uint32_t m_descriptorIncrement = 0;
    uint32_t m_descriptorOffset    = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantUpload;
    uint8_t* m_constantMapped = nullptr;
    size_t   m_constantOffset = 0;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoEquirect;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoIrradiance;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPrefilter;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBrdf;
    std::string m_loadedCompiledDir;
};

} /// @note namespace fbzz::renderer
