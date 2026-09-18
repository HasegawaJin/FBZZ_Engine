/// @file    Mesh.hpp
/// @brief   GPU メッシュと頂点フォーマットの定義。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// 静的メッシュとスキンメッシュの両方を扱う。
/// CPU 側頂点配列はデバッグや再生成用に必要な範囲だけ保持する。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ResourceHandle.hpp"
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::renderer {

struct Vertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
    /// 頂点カラー (RGBA)。既定の白は「色を持たない」と同じ意味になる。触るときは必ず読むこと。
    /// @note 末尾固定: 入力レイアウトは IShader が VS リフレクションの宣言順にオフセットを詰める。
    ///       色を末尾に置く限り COLOR 非宣言シェーダーの POSITION/NORMAL/TANGENT/TEXCOORD は
    ///       ずれない。途中へ挿すと全シェーダーのオフセットが一斉にずれる。
    /// @note float4 固定: リフレクションは型しか分からず RGBA8 圧縮には semantic 名の例外が要る。
    ///       COLOR は DebugDraw / ParticleVSIn が本物の float4 として使うため、名前で分岐すると
    ///       そちらが壊れる。1 頂点 16 バイトはこの一貫性の対価。
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

struct SkinnedVertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
    uint32_t boneIndices[4] = {};
    float boneWeights[4] = {};
};

/// DCC の BlendShape / Shape Key を基準頂点からの差分として保持する。
struct MorphTarget {
    std::string name;
    std::vector<math::Vector3> positionDeltas;
    std::vector<math::Vector3> normalDeltas;
    std::vector<math::Vector3> tangentDeltas;
};

struct Mesh {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    /// 確保済みの要素数。vertexCount / indexCount は «今描く数» で、こちらは «入る数»。
    /// @note 毎フレーム CPU で組み直すメッシュ (Sprite / LineRenderer の帯) は、頂点数が増減する
    ///       たびにバッファを作り直すと GPU リソースの生成・解放が毎フレーム走る。容量に収まって
    ///       いる間は Update で中身だけ差し替える。静的メッシュは既定 0 のままで実害はない。
    uint32_t vertexCapacity = 0;
    uint32_t indexCapacity  = 0;
    bool isSkinned = false;
    std::vector<Vertex>        cpuVertices;
    std::vector<SkinnedVertex> cpuSkinnedVertices;
    std::vector<uint32_t>      cpuIndices;
    std::vector<MorphTarget>   morphTargets;

    /// フラスタム/オクルージョンカリング用バウンディング球 (ローカル空間)。
    /// ComputeBounds() を呼ぶと cpuVertices / cpuSkinnedVertices から算出される。
    /// @note 球テストは AABB より分岐が少なく、ワールド変換も中心点変換+スケール乗算だけで済む。
    math::Vector3 boundsCenter  = {};
    float         boundsRadius  = 0.0f;
    /// ローカル AABB の半分の大きさ (boundsCenter からの各軸の伸び)。
    /// @note 球はカリング判定に十分だが「この形は球で近似してよいか」を判定できない。板ポリと
    ///       立方体は同じ半径の球になり得るが遮蔽者としての信頼度は正反対 (OcclusionCuller::
    ///       TestAndRaster の内接正方形の前提が崩れる)。最小半径成分と boundsRadius の比が材料。
    math::Vector3 boundsExtents = {};

    /// CPU 頂点配列からバウンディング球と AABB を計算する。
    /// メッシュ生成後・GPU アップロード前に一度だけ呼ぶ想定。
    void ComputeBounds();

    /// boundsExtents だけを CPU 頂点配列から埋める (球には触れない)。
    /// @note .mesh / .fzasset は球だけをヘッダーに持つため、そちらから読んだメッシュは AABB を
    ///       知らないまま「薄い形」と区別がつかない。フォーマットを変えずに補うため、頂点を
    ///       読み終えた直後にこれだけを走らせる。
    void ComputeBoundsExtents();
};

} // namespace fbzz::renderer
