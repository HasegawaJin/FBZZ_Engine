/// @file    InstanceBatch.hpp
/// @brief   連続する同一の描画を 1 回の Instanced Draw へ束ねる。
/// @author  Hasegawa Jin
/// @date    2026-09-20

/// @see Docs/design/gpu-instancing.md
/// @see Docs/design/skinned-crowds.md
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <vector>

namespace fbzz::renderer {

/// @brief インスタンス 1 個ぶんの可変データ。
/// @note LAYOUT: Assets/Shaders/Common/ObjectInstance.hlsli の ObjectInstance と一致させること。
/// @note ずらすと VS が別の行列を読み、物体が原点や無限遠へ飛ぶ。
struct PerInstanceData {
    math::Matrix4 world; ///< 頂点のローカル座標をワールド座標に変換する行列。
    math::Matrix4 worldInvTranspose; ///< 非一様スケールを含むワールド変換に合わせて法線を変換する逆転置行列。
};
static_assert(sizeof(PerInstanceData) == 128, "ObjectInstance.hlsli と同じ 128 バイトであること");
/// @note InstanceBatcher は b1 の載せ直しを memcmp で飛ばす。詰め物が入ると未初期化バイトの
/// @note 比較になり «同じ内容なのに毎回載せ直す» か、逆に取りこぼす。
static_assert(sizeof(PerObjectCB) == 144, "PerObjectCB に詰め物が入っていないこと");

/// @brief 2 つの DrawCall を「同じインスタンス束へ入れてよいか」で比べる。
/// @return Object/instance bindings may differ; deformed draws may also differ in output vertices when their source identity matches.
/// @note 「メッシュとマテリアルが同じなら束ねる」という鍵を手書きしない理由: テクスチャ 32 枠・
/// @note 定数バッファ 14 枠のどれか 1 つでも違えば絵が変わる。鍵を手書きすると枠を増やしたときに
/// @note 束ね条件だけが古くなり、別のテクスチャで描かれても絵から原因に辿りつけない。
[[nodiscard]] FBZZ_GRAPHICS_API bool IsSameInstanceBatch(const renderer::DrawCall& a, const renderer::DrawCall& b);

/// @brief 積まれた描画のうち連続して同一のものを束ね、まとめて Submit する。
/// @note 比較は直前の 1 件とだけ行う (O(n))。総当たりにしないのは、各パスが既にマテリアル順や
/// @note 深度順へ並べてあり、束ねられる描画は連続して現れるため。
/// @note 束ねるかどうかで描画順は変えない。並べ替えの損得は測ってから別に決める
/// @note (Docs/design/gpu-instancing.md «やらないこと»)。
class InstanceBatcher {
public:
    /// @param[in] baseShader 束ねてよい描画が使っているシェーダー。
    /// @param[in] instancedShader baseShader の変種。無効なら束ねず 1 件ずつ出す。
    /// @param[in] shadow true なら統計を影側 (statsShadowDrawCalls) へ積む。
    /// @note baseShader を受けるのは、同じループに別のシェーダーで描く物体が混ざるため
    /// @note (影の列には VS スキニングへ落ちた caster が混ざる)。シェーダーを見ずに差し替えると、
    /// @note 変種が想定していない頂点入力のまま描いてジオメトリが壊れる。
    FBZZ_GRAPHICS_API InstanceBatcher(RenderPassContext& ctx,
                    renderer::ResourceHandle<renderer::ShaderTag> baseShader,
                    renderer::ResourceHandle<renderer::ShaderTag> instancedShader,
                    bool shadow, const math::Frustum* gpuFrustum = nullptr);
    /// @note 溜めたまま捨てると描画が黙って消えるため、デストラクタでも吐き出す。
    FBZZ_GRAPHICS_API ~InstanceBatcher();

    InstanceBatcher(const InstanceBatcher&)            = delete;
    InstanceBatcher& operator=(const InstanceBatcher&) = delete;

    /// @brief 描画を 1 件積む。直前と束ねられなければ、溜まっていた束を先に吐き出す。
    /// @param[in] prototype b1 と instanceBuffer / instanceCount を空にした DrawCall。
    /// @param[in] object この物体の b1 の中身。objectParams.x != 0 (LOD 遷移中) は束ねない。
    /// @param[in] worldSphere 形状を覆うワールド空間の境界球。半径が負のときバッチを GPU で圧縮しない。
    FBZZ_GRAPHICS_API void Add(const renderer::DrawCall& prototype, const PerObjectCB& object,
        const math::Vector4& worldSphere = {0, 0, 0, -1});

    /// @brief 溜まっている束を吐き出す。描画先やビューポートを切り替える前に必ず呼ぶこと。
    FBZZ_GRAPHICS_API void Flush();

    /// @brief 「b1 に何を載せたか」の記憶を捨てる。
    /// @note 呼び出し側が handles.objectCB を自分で書き換えたときに呼ぶこと。載せ直しの省略は
    /// @note «最後に載せたのは自分» という前提に立っているので、外から書かれると次の単体描画が
    /// @note 更新を飛ばし、前の物体の姿勢で描かれる。
    void InvalidateObjectConstants() { m_hasLastObject = false; }

private:
    [[nodiscard]] bool CheckDeformedReadiness(ResourceHandle<BufferTag> output);
    const math::Frustum* m_gpuFrustum = nullptr; ///< GPU 選別用に参照するビューの視錐台。
    std::vector<math::Vector4> m_gpuBounds; ///< 保留中インスタンスに対応するワールド空間の境界球。
    FBZZ_GRAPHICS_API void SubmitOne(const renderer::DrawCall& call);
    /// @brief b1 を更新する。直前に載せた内容と同じなら何もしない。
    FBZZ_GRAPHICS_API void BindObjectConstants(renderer::DrawCall& call, const PerObjectCB& object);

    RenderPassContext&                            m_ctx; ///< 現在記録中の描画パスのコンテキスト。
    renderer::ResourceHandle<renderer::ShaderTag> m_baseShader; ///< 単体描画に使うシェーダーのハンドル。
    renderer::ResourceHandle<renderer::ShaderTag> m_instancedShader; ///< 複数のインスタンスをまとめて描くシェーダーのハンドル。
    bool                                          m_shadow = false; ///< 影の深度を描くバッチか。
    bool                                          m_reuseDeformedReadiness = false; ///< 変形後頂点の準備状態をバッチ内で再利用できるか。

    renderer::DrawCall       m_prototype{}; ///< 保留中の描画に共通する設定を保持する描画コマンド。
    std::vector<PerObjectCB> m_pending; ///< まとめて描画する各インスタンスのオブジェクト定数。
    renderer::ResourceHandle<renderer::ShaderTag> m_deformedShader; ///< 変形済みの頂点を入力として使うシェーダー。
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> m_deformedVertices; ///< 準備状態を確認した変形済み頂点バッファ。

    PerObjectCB m_lastObject{}; ///< 最後に送ったオブジェクト定数。再送の要否を判定する。
    bool        m_hasLastObject = false; ///< 比較に使える前回のオブジェクト定数があるか。
};

/// @brief インスタンスバッファの貸出プールを手放す。シーン切り替え・リソースリセットで呼ぶこと。
FBZZ_GRAPHICS_API void ReleaseInstanceBatchCaches(renderer::ResourceManager& resources);

} // namespace fbzz::renderer
