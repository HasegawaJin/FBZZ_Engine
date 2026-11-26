/// @file    DrawCall.hpp
/// @brief   レンダラーへ渡す描画要求データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 頂点・インデックス・シェーダー・定数バッファを ResourceHandle で参照する。
/// @note RenderSystem が生成し、IRenderer が ResourceManager で実体へ解決する。
#pragma once
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <array>
#include <cstdint>
#include <vector>
#include <memory>
#include "Graphics/Renderer/Visibility/RenderLayer.hpp"
#include "Graphics/Renderer/State/RenderState.hpp"
#include "Graphics/Renderer/Resources/ResourceHandle.hpp"

namespace fbzz::renderer {
class PreparedSkinningTicket;

/// @brief DrawCall::psBuffers が占めるピクセルシェーダー SRV の先頭レジスタ。
inline constexpr uint32_t kPsBufferBaseSlot = srv::PIXEL_BUFFER_FIRST;
/// @note VS粒子のt14/t15とは添字空間を分けた、追加のPS-only数値表。
inline constexpr uint32_t kAuxiliaryPsBufferBaseSlot = srv::AUXILIARY_FIRST;

struct DrawCall {
    /// @note 原 snapshot の出力 lease。通常 handle の退役後も捕捉済み native 頂点を保持する。
    std::shared_ptr<const PreparedSkinningTicket> preparedSkinning; ///< 元のスナップショットの変形頂点を保持するチケット。通常ハンドルの退役後も描画に使う。
    uint32_t preparedSkinningRow = 0; ///< preparedSkinning が保持する変形出力の添字。
    /// @note Original immutable mesh identity used only by automatic batching of distinct deformed outputs.
    ResourceHandle<BufferTag> deformationSource; ///< 変形出力の自動バッチ化で同じ元メッシュを識別する不変のハンドル。
    /// @note Every raw vertex buffer indexed by instance rows; DIRECT queue validation and transitions cover the whole list.
    std::vector<ResourceHandle<BufferTag>> indirectReadBuffers; ///< インスタンス行が読む raw 頂点バッファの全件。DIRECT キューの検証と状態遷移に使う。
    ResourceHandle<BufferTag> vertexBuffer; ///< 頂点入力に使う非所有ハンドル。
    ResourceHandle<BufferTag> indexBuffer; ///< インデックス付き描画で使う非所有ハンドル。
    ResourceHandle<ShaderTag> shader; ///< 使用する描画プログラムの非所有ハンドル。
    ResourceHandle<PipelineStateTag> pipelineState; ///< ブレンド、深度、ラスタライザ状態の非所有ハンドル。

    /// @brief 定数バッファ (b0〜b13)。両バックエンドで共通契約とし、DX12 は root CBV へ Submit 時
    /// @note        の GPU VA を記録する。
    /// @note       [5]=PostProcConstants(b5) [6]=AtmosphereConstants(b6) [7]=SkinningConstants(b7)
    std::array<ResourceHandle<ConstantBufferTag>, bindingCount::CONSTANT_BUFFERS> constantBuffers = {}; ///< 定数バッファ。添字は b レジスタ番号。

    /// @brief テクスチャ (t0〜t31)。
    std::array<ResourceHandle<TextureTag>, bindingCount::PIXEL_SRVS> textures = {}; ///< ピクセルシェーダー向けテクスチャ。添字は t レジスタ番号。
    /// @note 定数の bindless 添字から読むテクスチャ。スロットを消費せず状態遷移と寿命追跡へ渡す。
    std::array<ResourceHandle<TextureTag>, 2> indirectReadTextures{}; ///< 定数の bindless 添字から読むテクスチャ。状態遷移と寿命の追跡に使う。

    uint32_t indexCount  = 0; ///< インデックス付き描画で読むインデックス数。
    /// @note indexCount=0 かつ vertexCount>0 でインデックスなし描画 (フルスクリーントライアングル等)。
    uint32_t vertexCount = 0; ///< indexCount が 0 のときに描く頂点数。
    uint32_t startIndex  = 0; ///< indexBuffer の読み取り開始位置 [要素]。
    uint32_t baseVertex  = 0; ///< 各インデックスに足す vertexBuffer の基準位置 [頂点]。

    /// @brief GPU Instancing。instanceBuffer が有効なら instanceCount 1 以上で Instanced Draw。
    /// @note instanceBuffer は VS の t0 に StructuredBuffer<T> としてバインドされ、
    /// @note       HLSL 側で SV_InstanceID でインデックスしてインスタンスデータを取得する。
    uint32_t instanceCount = 1; ///< この要求が描くインスタンス数。
    ResourceHandle<StructuredBufferTag> instanceBuffer; ///< SV_InstanceID で参照するインスタンス別の入力。

    /// @brief VS-readable StructuredBuffer (t14〜t15)。GPU パーティクル等の頂点データを渡す。
    /// @note SV_VertexID ベースの描画は頂点バッファを持たず、StructuredBuffer から取り出す。
    /// @note       t14 を使うのはテクスチャ SRV (t0〜t13) と重複しないため。
    std::array<ResourceHandle<StructuredBufferTag>, bindingCount::BUFFER_RANGE> vsBuffers = {}; ///< 頂点シェーダーが読む構造化バッファ。添字 0 は SRV t14。

    /// @brief VS/MS-readable raw Buffer SRV (t14〜t15)。Mesh Shader が既存 VB/IB を読む用途にも使う。
    /// @note 同じ slot の vsBuffers と同時指定しない。両方を指定した場合、DX12 は描画を記録しない。
    std::array<ResourceHandle<BufferTag>, bindingCount::BUFFER_RANGE> vsRawBuffers = {}; ///< 頂点・メッシュシェーダーが読む raw バッファ。添字 0 は SRV t14。
    /// @note Mesh stage reads compacted candidate IDs in bindless vertex slot 3.
    ResourceHandle<StructuredBufferTag> meshletSelection; ///< 選別後のメッシュレット ID 列。メッシュシェーダーが bindless 頂点スロット 3 から読む。
    /// @note Bindless meshlet page table references these SRVs through descriptor indices.
    std::vector<ResourceHandle<StructuredBufferTag>> meshletPages; ///< bindless メッシュレットページ表から記述子添字で参照される SRV の全件。

    /// @brief PS-readable StructuredBuffer (t29〜t30)。クラスタライティングのライト配列と
    /// @note        インデックスリスト。
    /// @note vsBuffers / vsRawBuffers は VS/MS の t14/t15 専用で、DX12 側はピクセル SRV
    /// @note       テーブル (t0〜t31) の空き 2 枠へ差し込むだけでルートシグネチャの変更は不要。
    std::array<ResourceHandle<StructuredBufferTag>, bindingCount::BUFFER_RANGE> psBuffers = {}; ///< ピクセルシェーダーが読むクラスタ照明表。添字 0 は SRV t29。
    /// @note 同じ番号のtexturesと同時に設定しない。従来のクラスタ表t29/t30は維持する。
    std::array<ResourceHandle<StructuredBufferTag>, bindingCount::BUFFER_RANGE> auxiliaryPsBuffers = {}; ///< ピクセルシェーダー専用の補助表。同じレジスタの textures と排他。

    /// @brief GPU が生成した間接描画引数。各要素は 20B/24B DRAW_INDEXED または 12B DISPATCH_MESH。
    /// @note 全 command は同じ DrawCall の shader、root binding、render target、topology を共有する。
    /// @note 引数バッファは ExecuteIndirect の記録から GPU fence 完了まで生存させる。
    ResourceHandle<StructuredBufferTag> indirectArguments; ///< GPU 生成の間接描画引数を保持するバッファ。
    /// @brief indirectArguments 内の byte offset。ExecuteIndirect の引数形式に合わせ 4 byte 境界であること。
    uint64_t indirectArgumentOffset = 0; ///< indirectArguments の実行開始位置 [byte]。4 バイト境界。
    /// @brief 引数バッファから実行する共通 binding の command 数。
    uint32_t indirectCommandCount = 1; ///< 実行する、資源束縛が共通の間接コマンド数。
    /// @brief 各 indexed indirect command の先頭 DWORD を b15 root constant として渡す。
    /// @note true は 24 byte argument record 専用で、Mesh Shader dispatch と同時に指定できない。
    bool indirectInstanceOffset = false; ///< 間接インデックス描画の引数先頭からインスタンスの開始位置を取得する。

    /// @brief Mesh Shader の直接 dispatch または間接 dispatch 形式を選ぶ threadgroup X 数。
    /// @note indirectArguments が無効で X > 0 の場合は direct Mesh Shader dispatch。
    /// @note indirectArguments が有効なら X > 0 は DISPATCH_MESH、X = 0 は DRAW_INDEXED を選ぶ。
    /// @note Y/Z は indirect dispatch では引数データから取得し、直接 dispatch のときだけ使う。
    uint32_t meshGroupCountX = 0; ///< X 方向のグループ数。間接描画ではコマンド形式の選択に使う。
    uint32_t meshGroupCountY = 0; ///< 直接メッシュ描画の Y 方向グループ数。
    uint32_t meshGroupCountZ = 0; ///< 直接メッシュ描画の Z 方向グループ数。

    RenderLayer layer = RenderLayer::OPAQUE_LAYER; ///< 描画キューと並び順を選ぶレイヤー。
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST; ///< 頂点列をプリミティブへ組み立てる方式。
    /// @note インデックス付きストリップのインスタンス当たり三角形数。再開マーカーを含めない。
    uint32_t stripTriangleCount = 0; ///< インデックス付きストリップのインスタンス当たり三角形数。再開マーカーを含まない。
};

} // namespace fbzz::renderer
