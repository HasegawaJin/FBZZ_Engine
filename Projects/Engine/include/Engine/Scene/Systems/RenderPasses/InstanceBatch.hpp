/// @file    InstanceBatch.hpp
/// @brief   連続する同一の描画を 1 回の Instanced Draw へ束ねる。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// @see Docs/design/gpu-instancing.md
#pragma once
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Math/Matrix4.hpp>
#include <vector>

namespace fbzz::scene {

/// @brief インスタンス 1 個ぶんの可変データ。
/// @note LAYOUT: Assets/Shaders/Common/ObjectInstance.hlsli の ObjectInstance と一致させること。
///       ずらすと VS が別の行列を読み、物体が原点や無限遠へ飛ぶ。
struct PerInstanceData {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
};
static_assert(sizeof(PerInstanceData) == 128, "ObjectInstance.hlsli と同じ 128 バイトであること");
/// @note InstanceBatcher は b1 の載せ直しを memcmp で飛ばす。詰め物が入ると未初期化バイトの
///       比較になり «同じ内容なのに毎回載せ直す» か、逆に取りこぼす。
static_assert(sizeof(PerObjectCB) == 144, "PerObjectCB に詰め物が入っていないこと");

/// @brief 2 つの DrawCall を「同じインスタンス束へ入れてよいか」で比べる。
/// @return b1 (ObjectConstants) と instanceBuffer / instanceCount 以外がすべて一致すれば true。
/// @note 「メッシュとマテリアルが同じなら束ねる」という鍵を手書きしない理由: テクスチャ 32 枠・
///       定数バッファ 14 枠のどれか 1 つでも違えば絵が変わる。鍵を手書きすると枠を増やしたときに
///       束ね条件だけが古くなり、別のテクスチャで描かれても絵から原因に辿りつけない。
[[nodiscard]] bool IsSameInstanceBatch(const renderer::DrawCall& a, const renderer::DrawCall& b);

/// @brief 積まれた描画のうち連続して同一のものを束ね、まとめて Submit する。
/// @note 比較は直前の 1 件とだけ行う (O(n))。総当たりにしないのは、各パスが既にマテリアル順や
///       深度順へ並べてあり、束ねられる描画は連続して現れるため。
/// @note 束ねるかどうかで描画順は変えない。並べ替えの損得は測ってから別に決める
///       (Docs/design/gpu-instancing.md «やらないこと»)。
class InstanceBatcher {
public:
    /// @param baseShader      束ねてよい描画が使っているシェーダー。
    /// @param instancedShader baseShader の変種。無効なら束ねず 1 件ずつ出す。
    /// @param shadow          true なら統計を影側 (statsShadowDrawCalls) へ積む。
    /// @note baseShader を受けるのは、同じループに別のシェーダーで描く物体が混ざるため
    ///       (影の列には VS スキニングへ落ちた caster が混ざる)。シェーダーを見ずに差し替えると、
    ///       変種が想定していない頂点入力のまま描いてジオメトリが壊れる。
    InstanceBatcher(RenderPassContext& ctx,
                    renderer::ResourceHandle<renderer::ShaderTag> baseShader,
                    renderer::ResourceHandle<renderer::ShaderTag> instancedShader,
                    bool shadow);
    /// @note 溜めたまま捨てると描画が黙って消えるため、デストラクタでも吐き出す。
    ~InstanceBatcher();

    InstanceBatcher(const InstanceBatcher&)            = delete;
    InstanceBatcher& operator=(const InstanceBatcher&) = delete;

    /// @brief 描画を 1 件積む。直前と束ねられなければ、溜まっていた束を先に吐き出す。
    /// @param prototype b1 と instanceBuffer / instanceCount を空にした DrawCall。
    /// @param object この物体の b1 の中身。objectParams.x != 0 (LOD 遷移中) は束ねない。
    void Add(const renderer::DrawCall& prototype, const PerObjectCB& object);

    /// @brief 溜まっている束を吐き出す。描画先やビューポートを切り替える前に必ず呼ぶこと。
    void Flush();

    /// @brief 「b1 に何を載せたか」の記憶を捨てる。
    /// @note 呼び出し側が handles.objectCB を自分で書き換えたときに呼ぶこと。載せ直しの省略は
    ///       «最後に載せたのは自分» という前提に立っているので、外から書かれると次の単体描画が
    ///       更新を飛ばし、前の物体の姿勢で描かれる。
    void InvalidateObjectConstants() { m_hasLastObject = false; }

private:
    void SubmitOne(const renderer::DrawCall& call);
    /// @brief b1 を更新する。直前に載せた内容と同じなら何もしない。
    void BindObjectConstants(renderer::DrawCall& call, const PerObjectCB& object);

    RenderPassContext&                            m_ctx;
    renderer::ResourceHandle<renderer::ShaderTag> m_baseShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_instancedShader;
    bool                                          m_shadow = false;

    renderer::DrawCall       m_prototype{};
    std::vector<PerObjectCB> m_pending;

    PerObjectCB m_lastObject{};
    bool        m_hasLastObject = false;
};

/// @brief インスタンスバッファの貸出プールを手放す。シーン切り替え・リソースリセットで呼ぶこと。
void ReleaseInstanceBatchCaches(renderer::ResourceManager& resources);

} // namespace fbzz::scene
