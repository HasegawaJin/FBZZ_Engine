// FBZZ Engine
// DebugDraw.hpp | fbzz::renderer
// ワイヤーフレームのデバッグ描画ユーティリティ
// Line / Box / Sphere / Capsule をフレーム内バッチとして集めて一括送信する。
// 描画は BeginFrame から Flush までの間だけ有効。
#pragma once

#include "IRenderer.hpp"
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Matrix4.hpp>
#include <cstddef>

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
    // フレーム末尾で呼ぶ。蓄積した頂点を一括 Submit し、バッチをクリアする。
    // Submit は即時実行のため、1 フレーム内で複数回呼んでも安全 (溢れそうな時の中間 Flush 用)。
    static void Flush();

    // 深度なし線分バッチの現在の頂点数と上限。
    // WHY: バッチが満杯になると以降の Line() は黙って捨てられる。大量のラインを出すパス
    //      (コライダー可視化・NavMesh 等) が「溢れる前に Flush する」判断をするために公開する。
    [[nodiscard]] static size_t PendingLineVertices();
    [[nodiscard]] static size_t MaxBatchVertices();

    static void Line(IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color = {1,1,1,1});
    // 深度テストありの線分。シーンジオメトリに正しく遮蔽される。
    // WHY: Line (深度なし) はコライダーギズモ等をメッシュ越しに見せるための仕様。
    //      一方でエディターのグリッド線のような「世界に置かれた線」は、メッシュの
    //      手前に浮いて見えると空間の前後関係が壊れるため、遮蔽される版を分ける。
    static void LineDepthTested(IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                                const math::Vector4& color = {1,1,1,1});
    static void Box(IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                    const math::Vector4& color = {0,1,0,1});
    static void Box(IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                    const math::Quaternion& rotation, const math::Vector4& color = {0,1,0,1});
    static void Sphere(IRenderer& r, const math::Vector3& center, float radius,
                       const math::Vector4& color = {0,1,0,1});
    static void Capsule(IRenderer& r, const math::Vector3& center, float radius, float halfHeight,
                        const math::Vector4& color = {0,1,0,1});
    static void Capsule(IRenderer& r, const math::Vector3& center, float radius, float halfHeight,
                        const math::Quaternion& rotation, const math::Vector4& color = {0,1,0,1});

    // from → to の方向を示す矢印 (シャフト + コーン型ヘッド)。
    // headLength: ヘッド部分のワールド単位の長さ (シャフト全体を超えるとクランプする)
    // headRadius: ヘッドの底面半径
    static void Arrow(IRenderer& r,
                      const math::Vector3& from, const math::Vector3& to,
                      float headLength = 0.2f, float headRadius = 0.05f,
                      const math::Vector4& color = {1,1,0,1});

    // ワイヤーフレームのコーン。
    // apex: 頂点、direction: 底面方向の正規化ベクトル、height: 高さ、baseRadius: 底面半径
    static void Cone(IRenderer& r,
                     const math::Vector3& apex, const math::Vector3& direction,
                     float height, float baseRadius,
                     const math::Vector4& color = {1,1,0,1});

    // 凸ポリゴンを半透明塗りつぶしで描く (fan 三角分割、TRIANGLE_LIST + ALPHA_BLEND)
    static void FilledPolygon(IRenderer& r, const math::Vector3* vertices, size_t count,
                              const math::Vector4& color = {0.12f, 0.35f, 0.90f, 0.30f});
};

} // namespace fbzz::renderer
