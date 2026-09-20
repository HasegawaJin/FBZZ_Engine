/// @file    GeometryRoute.hpp
/// @brief   不透明・半透明のジオメトリを GBuffer と Forward のどちらへ流すかの唯一の規則。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// @see Docs/design/pipeline-boundary.md
#pragma once
#include <Engine/Renderer/RenderState.hpp>
#include <string_view>

namespace fbzz::scene {

struct MaterialSlot;

/// @brief 1 つの描画をどの経路で描くか。
/// @note 3 つは互いに排他で、どの描画もちょうど 1 つに落ちる。分岐を各パスへ書き写すと
///       «どれにも当たらない» / «2 つに当たる» が作れてしまい、物が消えるか二重に描かれる。
enum class GeometryRoute : std::uint8_t {
    GBuffer,            ///< @brief GBuffer へ書き、DeferredLighting が後で解く
    ForwardOpaque,      ///< @brief 不透明だが Forward で完全評価する
    ForwardTransparent, ///< @brief 半透明。深度順に後から描く
};

/// @brief 経路を決めるのに要る事実。アセットを引いた «結果» だけを持つ。
/// @note 政策 (どう決めるか) と事実集め (材質を引く) を分ける。政策が純粋関数なら、
///       シーンもアセットも無しに全組み合わせをテストで固定できる。
struct GeometryRouteInput {
    renderer::BlendMode blend = renderer::BlendMode::OPAQUE_BLEND;
    /// @brief GBuffer 経路が動いているか (Deferred / Deferred+)。
    bool gbufferPipeline = false;
    /// @brief この材質のシェーダーが GBuffer 相当か。@see IsGBufferEquivalentShader
    bool gbufferEquivalentShader = false;
    /// @brief clearcoat / sheen / anisotropy / cloth のいずれかを持つか。
    /// @note GBuffer 2 枚には拡張ローブも接線基底も入らない。
    bool advancedLobe = false;
};

/// @brief 経路を決める。上から順に当てはめ、最初に当たったものを採る。
/// @note 順序に意味がある。半透明の判定を後ろに回すと、半透明のカスタム材質が
///       ForwardOpaque へ落ちて深度順ソートから外れる。
/// @see Docs/design/pipeline-boundary.md §2
[[nodiscard]] constexpr GeometryRoute ResolveGeometryRoute(const GeometryRouteInput& input)
{
    if (input.blend != renderer::BlendMode::OPAQUE_BLEND) return GeometryRoute::ForwardTransparent;
    if (!input.gbufferPipeline)                           return GeometryRoute::ForwardOpaque;
    if (!input.gbufferEquivalentShader)                   return GeometryRoute::ForwardOpaque;
    if (input.advancedLobe)                               return GeometryRoute::ForwardOpaque;
    return GeometryRoute::GBuffer;
}

/// @brief このシェーダーを GBuffer.hlsl へ置き換えても絵が変わらないか。
/// @param shaderPath `.mat` の shader。空文字は Fallback を指す (= GBuffer 相当)。
/// @note ファイル名で比べる。シェーダーは `Assets/` と `<Project>/Assets/` の 2 本立てで、
///       参照は `guid:` にもパスにもなるため、絶対パスで比べると同じものが別物に見える。
/// @note 知らないシェーダーは false。ユーザーが書いたカスタムシェーダーは Forward (常に正しいが
///       遅いほう) へ倒れる。安全な側を既定にする。
/// @see Docs/design/pipeline-boundary.md §2.1
[[nodiscard]] bool IsGBufferEquivalentShader(std::string_view shaderPath);

/// @brief 材質スロットから経路を決める。各描画パスはこちらを呼ぶ。
/// @note アセットを引くので純粋関数ではない。規則そのものは上の ResolveGeometryRoute が持つ。
/// @note スキンドかどうかを受けない。«スキンドだから Forward» という規則を持たないことが
///       この設計の眼目で、引数にあると «いつか分岐するもの» として読まれる
///       (Docs/design/pipeline-boundary.md §3)。
[[nodiscard]] GeometryRoute ResolveGeometryRoute(const MaterialSlot& slot, bool gbufferPipeline);

} // namespace fbzz::scene
