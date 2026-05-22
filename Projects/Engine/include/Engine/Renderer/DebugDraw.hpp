// FBZZ Engine
// DebugDraw.hpp | fbzz::renderer
// ワイヤーフレームのデバッグ描画ユーティリティ (Line / Box / Sphere / Capsule)
#pragma once

#include "IRenderer.hpp"
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Matrix4.hpp>

namespace fbzz::renderer {

class ResourceManager;

// 使い方:
//   毎フレーム先頭で BeginFrame() を呼び、描画命令を積み、フレーム末尾で Flush() を呼ぶ。
//   Line / Box / Sphere / Capsule は内部バッチに追記するだけで即座には描画しない。
//   Flush() がバッチをまとめて LINE_LIST DrawCall として Submit する。
class DebugDraw {
public:
    // フレーム先頭で呼ぶ。リソースを遅延初期化し、カメラ VP 行列を設定する
    static void BeginFrame(IRenderer& r, ResourceManager& resources, const math::Matrix4& viewProjection);
    // フレーム末尾で呼ぶ。蓄積した頂点を一括 Submit し、バッチをクリアする
    static void Flush();

    static void Line(IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color = {1,1,1,1});
    static void Box(IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                    const math::Vector4& color = {0,1,0,1});
    static void Box(IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                    const math::Quaternion& rotation, const math::Vector4& color = {0,1,0,1});
    static void Sphere(IRenderer& r, const math::Vector3& center, float radius,
                       const math::Vector4& color = {0,1,0,1});
    static void Capsule(IRenderer& r, const math::Vector3& center, float radius, float halfHeight,
                        const math::Vector4& color = {0,1,0,1});
};

} // namespace fbzz::renderer
