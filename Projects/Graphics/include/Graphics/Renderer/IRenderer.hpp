/// @file    IRenderer.hpp
/// @brief   Renderer バックエンドの抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 上位レイヤーはバックエンド具象を直接参照せず、このインターフェースだけを使う。
/// @note リソース生成は ResourceManager に閉じ、描画 API は Submit / Dispatch に集約する。
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ComputeCall.hpp"
#include "DrawCall.hpp"
#include "Format.hpp"
#include "IIblBaker.hpp"
#include "IBuffer.hpp"
#include "IConstantBuffer.hpp"
#include "IStructuredBuffer.hpp"
#include "IPipelineState.hpp"
#include "IRenderTarget.hpp"
#include "IShader.hpp"
#include "ITexture.hpp"
#include "RenderState.hpp"
#include "ResourceHandle.hpp"
#include "SamplerMode.hpp"
#include <Math/Vector4.hpp>

namespace fbzz::renderer {

class ResourceManager;

/// @note GPU プロファイリング 1 パス分の結果。
/// @note IRenderer を経由することで上位レイヤーが具象を知らずに GPU 時間を取得できる。
struct GpuPassProfile {
    std::string name;
    double gpuMs = 0.0;
};

/// @note RenderTarget 内部 SRV のどちらを TextureTag 化するかを表す。
/// @note bool 引数では Color / Depth の意味が呼び出し側から読めず、誤指定に気づきにくいため。
enum class RenderTargetTextureKind : uint8_t {
    Color,
    Depth,
};

class IRenderer {
public:
    virtual ~IRenderer() = default;

    /// @note アクティブな描画バックエンドの表示名 ("DirectX 11" / "DirectX 12")。
    /// @note ウィンドウタイトル等の UI 表示用。上位が具象型へダウンキャストしないための情報提供 API。
    virtual const char* GetBackendName() const { return "Unknown"; }

    /// @note GPU バックエンドが保持するパイプライン参照を解除し、デバイス破棄前の状態を確定する。
    /// @note バインド中のリソースはバックエンド側に参照が残るため、ResourceManager が
    /// @note       ネイティブリソースを破棄する前に明示的な終了処理が要る。
    virtual void Shutdown() = 0;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    virtual void Submit(const DrawCall& call, ResourceManager& resources) = 0;
    /// @note 診断画像を描き、呼び出し前の描画先・ビューポート・シザーへ戻す。
    /// @note パス境界でのみ呼ぶ。後続の Submit は自身の描画状態を束縛すること。
    /// @note 未対応のバックエンドでは false を返す。
    virtual bool RenderDebugPreview(const DrawCall& /*call*/,
                                    ResourceHandle<RenderTargetTag> /*target*/,
                                    ResourceManager& /*resources*/) { return false; }
    virtual void Dispatch(const ComputeCall& call, ResourceManager& resources) = 0;
    /// @note 相互依存しない Dispatch 群の UAV バリアをバッチ末尾へまとめる。
    /// @note スキニングのように各 Dispatch が別バッファへ書くパスでは、Dispatch ごとの
    /// @note       ResourceBarrier 呼び出しは不要。未対応バックエンドは既定の no-op でよい。
    virtual void BeginComputeBatch() {}
    virtual void EndComputeBatch() {}

    /// @brief ここから EndAsyncCompute までの Dispatch を非同期コンピュートキューへ記録する。
    /// @return 非同期キューへ載せたなら true。false のときは何も変わらず描画キューのまま続く。
    /// @pre 区間の中で Submit (描画) を呼ばないこと。描画先は区間の外で束縛し直される。
    /// @note 呼ぶ側は true / false のどちらでも同じ結果になるように書くこと。使えるかどうかは
    /// @note       機械と設定で変わり、絵が変わってはいけない。
    /// @note 区間へ入る時点で描画の列を割るため、直前までの描画がそこで GPU へ投入される。
    /// @see Docs/design/async-compute.md
    virtual bool BeginAsyncCompute(ResourceManager& /*resources*/) { return false; }
    /// @brief 非同期区間を閉じ、以降の描画がその完了を待つようにする。
    /// @note BeginAsyncCompute が false を返した場合も呼んでよい (no-op)。
    virtual void EndAsyncCompute() {}

    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    /// @note Present の垂直同期。既定は無効。
    /// @note フレームレート制御は Time::targetFps に任せる。Present の待ちを描画同期に混ぜると
    /// @note       プロファイラ上で描画コストと区別できなくなる。
    /// @note 有効にすると tearing 許可フラグは自動的に落ちる (併用は DXGI が拒否する)。
    virtual void SetVSync(bool /*enabled*/) {}
    [[nodiscard]] virtual bool GetVSync() const { return false; }

    virtual void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) = 0;
    /// @brief 束縛中の RT の深度を最遠値へ戻す (RenderTargetDesc::reversedZ なら 0、それ以外は 1)。
    /// @note 値を引数で受けないのは、RT の深度の向きと食い違ったクリアで深度テストが全滅するため。
    virtual void ClearDepth() = 0;

    /// @note SetViewport — 現在の描画先の一部矩形だけへ描くようビューポートを絞る。
    /// @note カスケードシャドウはシャドウマップを 2x2 タイルに分け、カスケードごとに描き込む
    /// @note       (アトラス)。深度テクスチャは 1 本のまま SRV 1 本で全カスケードを引ける。
    /// @note SetRenderTarget は呼ぶたびにビューポートを RT 全体へ戻す。
    /// @note       「SetRenderTarget → SetViewport」の順で呼び、絞った範囲は次の SetRenderTarget まで有効。
    virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

    /// @note SetRenderTargetFace — キューブマップ RT の 1 面 (+mip) を描画先にバインドする。
    /// @note 空連動 IBL の SkyCapture が空ドームを 6 面それぞれの向きで描くために使う。
    /// @note       未対応バックエンドは no-op でよい。CreateCubemapRenderTarget 以外の RT を渡した
    /// @note       場合の挙動は実装依存。
    virtual void SetRenderTargetFace(ResourceHandle<RenderTargetTag> /*rt*/, uint32_t /*face*/,
                                     uint32_t /*mip*/, ResourceManager& /*resources*/) {}

    /// @note かつてここに SetSampler(slot, mode) があったが削除した。サンプラーはレジスタごとに
    /// @note       意味を 1 つ固定する規約になり、正本は Assets/Shaders/Common/Binding.hlsli の SAMPLER_*
    /// @note       とバックエンド側の固定テーブル (DX12 は Root Signature の静的サンプラー) の対。
    /// @note DX12 は静的サンプラーを Root Signature へ焼き込むため 1 レジスタに 1 つの意味しか
    /// @note       持てず、動的差し替えは no-op のまま誰も気づかなかった (全画面パスの s0 が DX12 では
    /// @note       WRAP のせいで画面端が回り込む事故があった)。正本を 1 つにして構造的に防ぐ。

    /// @note GPU プロファイリング。未対応バックエンドは no-op のままでよい。
    /// @note タイムスタンプクエリで非同期に計測するため、GpuProfCollect() を呼んだ時点で
    /// @note       数フレーム前の結果が確定する (即値ではない)。
    virtual void GpuProfBeginFrame()                      {}
    virtual void GpuProfEndFrame()                        {}
    virtual void GpuProfBeginPass(const char* /*name*/)   {}
    virtual void GpuProfEndPass(const char* /*name*/)     {}
    virtual void GpuProfCollect()                         {}
    virtual const std::vector<GpuPassProfile>& GpuProfGetResults() const
    {
        static const std::vector<GpuPassProfile> s_empty;
        return s_empty;
    }

    /// @note BakeSkyLight — キャプチャ済み空キューブマップ (envCubeRT) を irradiance / prefilter キューブへ
    /// @note 畳み込み、それぞれを ITexture (TextureCube SRV) として返す。空連動 IBL の runtime 畳み込み経路。
    /// @note 畳み込み Compute は面ごとの Texture2DArray UAV を要求するバックエンド固有処理のため、
    /// @note       抽象 IRenderer は入口だけ提供する。返した ITexture は呼び出し側が
    /// @note       ResourceManager::RegisterTexture で所有する。未対応なら false。
    virtual bool BakeSkyLight(ResourceHandle<RenderTargetTag> /*envCubeRT*/, ResourceManager& /*resources*/,
                              uint32_t /*irradianceSize*/, uint32_t /*prefilterSize*/,
                              uint32_t /*prefilterMips*/, uint32_t /*sampleCount*/,
                              std::unique_ptr<ITexture>& /*outIrradiance*/,
                              std::unique_ptr<ITexture>& /*outPrefilter*/) { return false; }

    /// @note IBL ベイク処理の実装を返す (Editor 専用)。未対応のバックエンドは nullptr を返してよい。
    /// @note IblBaker はバックエンド固有の UAV 操作を必要とするため IRenderer の factory 経由で
    /// @note       提供し、Editor が具象レンダラーへ直接ダウンキャストしなくて済むようにする。
    virtual std::unique_ptr<IIblBaker> CreateIblBaker() { return nullptr; }

    /// @note CaptureRenderTargetToPng — 指定 RenderTarget のカラーを PNG バイト列として CPU へ読み戻す。
    /// @note AI 連携 (MCP viewport.capture) が Scene View を Claude に見せるために使う。GPU 読み戻し・
    /// @note       PNG エンコードはバックエンド固有 (DX12: CommandQueue + DirectXTex) のため入口だけ提供する。
    /// @note 未対応バックエンドは false。outPng は PNG 全体のバイト列、out{Width,Height} は
    /// @note       実 RT サイズ (要求サイズではない)。
    virtual bool CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> /*rt*/, ResourceManager& /*resources*/,
                                          std::vector<uint8_t>& /*outPng*/,
                                          uint32_t& /*outWidth*/, uint32_t& /*outHeight*/) { return false; }

    /// @note CaptureRenderTargetToLinearRGBA — 同じ RT を「絵」ではなく「数値」として読み戻す。
    /// @note PNG は 8bit UNORM へクランプされ、白飛びか単に明るいのかがエンコード時点で失われる。
    /// @note       露出・画面占有・動きの量を機械的に判定するには HDR の線形値が要る (vfx.previewMetrics が使う)。
    /// @note outRgba は width*height*4 の行優先 float 列。トーンマップもガンマ変換も行わない。
    virtual bool CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> /*rt*/, ResourceManager& /*resources*/,
                                                 std::vector<float>& /*outRgba*/,
                                                 uint32_t& /*outWidth*/, uint32_t& /*outHeight*/) { return false; }

private:
    friend class ResourceManager;

    virtual std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) = 0;
    /// @note CS が書き込み、IA が頂点として読むバッファ。コンピュートスキニングの出力先。
    /// @note スキニング結果を 1 度だけ計算してシャドウ・GBuffer・Forward で共有するため、同じ
    /// @note       バッファに UAV 書き込みと頂点入力の両方を許す。未対応バックエンドは nullptr を
    /// @note       返してよい (呼び出し側は VS スキニングへフォールバックする)。
    virtual std::unique_ptr<IBuffer> CreateNativeGpuWritableVertexBuffer(size_t /*sizeBytes*/, uint32_t /*stride*/) { return nullptr; }
    virtual std::unique_ptr<IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) = 0;
    virtual std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t sizeBytes) = 0;
    virtual std::unique_ptr<IShader> CreateNativeShader(const std::string& path) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeTexture(const std::string& path) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeTextureFromData(const uint8_t* rgba, uint32_t width, uint32_t height) = 0;
    /// @note CPU で焼いたミップ連鎖からテクスチャを作る。未対応バックエンドは nullptr を返してよい。
    /// @note 引数は 0 段目から順に並んだ RGBA8 (各段の行ピッチは width*4 固定)。ドライバの自動生成に
    /// @note       任せないのは、勾配を格納したテクスチャのように «どう縮小すれば正しいか» を呼び出し側
    /// @note       しか知らない場合があるため。
    virtual std::unique_ptr<ITexture> CreateNativeTextureFromDataMips(
        const TextureMipData* /*mips*/, uint32_t /*mipCount*/) { return nullptr; }
    /// @note CreateNativeTextureFromDataMips の待たない版。転送を投入して戻る。
    /// @param outUploadToken 転送完了の判定に IsUploadComplete へ渡す値。0 は完了済み。
    /// @note mips は戻った時点で手放してよい (転送元は内部の upload メモリへ複製済み)。
    /// @note 既定は同期版へ倒す。非同期転送を持たないバックエンドもそのまま動く。
    virtual std::unique_ptr<ITexture> CreateNativeTextureFromDataMipsAsync(
        const TextureMipData* mips, uint32_t mipCount, uint64_t& outUploadToken)
    {
        outUploadToken = 0;
        return CreateNativeTextureFromDataMips(mips, mipCount);
    }
    /// @note 非同期転送が GPU 上で完了したか。CPU は待たない。
    virtual bool IsUploadComplete(uint64_t /*uploadToken*/) const { return true; }
    virtual std::unique_ptr<ITexture> CreateNativeTexture3DFromData(
        const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeTextureFromRenderTarget(
        IRenderTarget& rt,
        uint32_t index,
        RenderTargetTextureKind kind) = 0;
    virtual std::unique_ptr<IPipelineState> CreateNativePipelineState(const PipelineStateDesc& desc) = 0;
    virtual std::unique_ptr<IRenderTarget> CreateNativeRenderTarget(uint32_t width, uint32_t height,
                                                                    const RenderTargetDesc& desc) = 0;
    /// @note 6 面キューブマップ描画先。未対応バックエンドは nullptr を返してよい。
    virtual std::unique_ptr<IRenderTarget> CreateNativeCubemapRenderTarget(uint32_t /*size*/, uint32_t /*mipCount*/) { return nullptr; }
    /// @note キューブマップ RT の TextureCube SRV を ITexture 化する (TextureTag として束縛可能にする)。
    virtual std::unique_ptr<ITexture> CreateNativeCubeTextureFromRenderTarget(IRenderTarget& /*rt*/) { return nullptr; }
    virtual std::unique_ptr<ITexture> CreateNativeComputeTexture(uint32_t width, uint32_t height) = 0;
    /// @note CS が RWTexture3D として書き、後段が Texture3D として読むボリューム (フロクセル霧)。
    /// @note 未対応バックエンドは nullptr を返してよい。呼び出し側は機能そのものを落とすこと。
    virtual std::unique_ptr<ITexture> CreateNativeComputeTexture3D(
        uint32_t /*width*/, uint32_t /*height*/, uint32_t /*depth*/) { return nullptr; }
    /// @note CPU から矩形単位で書き換えられるテクスチャ。ITexture::UpdateRegion と対で使う。
    /// @note 中身は未初期化ではなくゼロクリアされた状態で返すこと。
    /// @note フォントの動的アトラス (使われたグリフだけを実行時にラスタライズして貼る) が要求する。
    /// @note       Immutable な CreateNativeTextureFromData だと 1 グリフ増えるたびに全体を作り直し
    /// @note       ハンドルも毎回変わる。未対応バックエンドは nullptr でよい (呼び出し側は静的アトラスへ縮退)。
    virtual std::unique_ptr<ITexture> CreateNativeDynamicTexture(
        uint32_t /*width*/, uint32_t /*height*/, DynamicTextureFormat /*format*/) { return nullptr; }
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) = 0;
    /// @note 初期データだけを持つ GPU ローカル SRV。専用経路がないバックエンドは通常の SRV へ縮退する。
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeGpuLocalStructuredBuffer(
        const void* data, uint32_t elementCount, uint32_t stride)
    {
        return CreateNativeStructuredBuffer(data, elementCount, stride);
    }
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) = 0;

    /// @note 旧シェーダーの破棄前に GPU 使用と依存キャッシュを解消する。安全に切り替えられなければ false。
    virtual bool PrepareShaderReload() { return true; }
};

} /// @note namespace fbzz::renderer
