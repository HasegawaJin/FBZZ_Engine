/// @file    SceneRenderResources.hpp
/// @brief   シーンのコンポーネントが個体ごとに確保した GPU リソースの一括返却。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

#include <Engine/Scene/Entity.hpp>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;

/// コンポーネントが «その個体専用» に確保した GPU リソースを ResourceManager へ返す。
///
/// WHY 要るか:
///   Scene を畳んでもコンポーネントが持っていた ResourceHandle の実体は ResourceManager 側に残る。
///   Editor の Play → Stop はシーンを丸ごと読み直すため、繰り返すたびに前のシーンぶんの
///   定数バッファ・頂点バッファ・キューブマップが積み上がっていた。
/// NOTE: パスキャッシュ由来の共有リソース (LoadTexture / LoadShader の結果) は返さない。
///       同じハンドルを他のシーンや Editor UI が使い続けている。
void ReleaseSceneOwnedGpuResources(Scene& scene, renderer::ResourceManager& resources);

/// 1 エンティティぶんだけ同じ返却を行う。GameObject を畳む直前に呼ぶ。
void ReleaseEntityOwnedGpuResources(Scene& scene, EntityID id, renderer::ResourceManager& resources);

/// 複製直後のエンティティから «コピー元と同じハンドル» を捨てる。
///
/// WHY 要るか: Component 配列の複製は値コピーなので、GPU ハンドルまで写る。
///     そのままだと複製元と複製先が 1 本のバッファを共有し、毎フレーム互いのポーズを
///     上書きし合ううえ、片方を畳んだ瞬間にもう片方の参照先が消える。
///     所有者は複製元なので、ここでは «返さずに手放す» のが正しい。
void ClearDuplicatedGpuHandles(Scene& scene, EntityID id);

} // namespace fbzz::scene
