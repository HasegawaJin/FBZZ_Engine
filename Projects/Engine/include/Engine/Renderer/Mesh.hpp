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
    // 頂点カラー (RGBA)。既定の白は「色を持たない」と同じ意味になる。
    //
    // WHY 必ず末尾に置くか (触るときは必ず読むこと):
    //   入力レイアウトは DX11Shader / DX12Shader が VS のリフレクション結果を
    //   «宣言順に詰めて» 組み立てる。つまり各要素のオフセットはシェーダー側の
    //   宣言順だけで決まり、この構造体とは独立に計算される。色を末尾に置く限り、
    //   COLOR を宣言しないシェーダーの POSITION/NORMAL/TANGENT/TEXCOORD の
    //   オフセットは変わらないため、既存シェーダーは 1 行も触らずに動き続ける。
    //   途中へ挿すと全シェーダーのオフセットが一斉にずれる。
    //
    // WHY float4 で持つか (RGBA8 に圧縮しないか):
    //   リフレクションは「float4 と宣言されている」ことしか分からず、4 バイトへ
    //   圧縮するには semantic 名で例外を作る必要がある。だが COLOR は DebugDraw と
    //   ParticleVSIn が本物の float4 として既に使っており、名前で分岐すると
    //   そちらが壊れる。1 頂点 16 バイトはこの一貫性の対価。
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

// DCC の BlendShape / Shape Key を基準頂点からの差分として保持する。
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
    // 確保済みの要素数。vertexCount / indexCount は «今描く数» で、こちらは «入る数»。
    //
    // WHY 分けて持つか: 毎フレーム CPU で組み直すメッシュ (Sprite / LineRenderer の帯) は、
    //     頂点数が増減するたびにバッファを作り直すと GPU リソースの生成と解放が
    //     毎フレーム走る。容量に収まっている間は Update で中身だけ差し替えたい。
    //     静的メッシュは 1 度確保したきりなので、既定の 0 のままでも実害はない
    //     (最初の書き込みで容量が入る)。
    uint32_t vertexCapacity = 0;
    uint32_t indexCapacity  = 0;
    bool isSkinned = false;
    std::vector<Vertex>        cpuVertices;
    std::vector<SkinnedVertex> cpuSkinnedVertices;
    std::vector<uint32_t>      cpuIndices;
    std::vector<MorphTarget>   morphTargets;

    // フラスタム/オクルージョンカリング用バウンディング球 (ローカル空間)
    // ComputeBounds() を呼ぶと cpuVertices / cpuSkinnedVertices から算出される。
    // WHY: 球テストは AABB テストより分岐が少なく、ワールド変換も中心点変換+スケール乗算だけで済む。
    math::Vector3 boundsCenter  = {};
    float         boundsRadius  = 0.0f;
    // ローカル AABB の半分の大きさ (boundsCenter からの各軸の伸び)。
    // WHY 球と別に持つか: 球はカリング判定には十分だが、「この形は球で近似してよいか」を
    //     判定できない。板ポリと立方体は同じ半径の球になり得るのに、遮蔽者としての
    //     信頼度は正反対 (OcclusionCuller::TestAndRaster の内接正方形の前提が崩れる)。
    //     最小の半径成分と boundsRadius の比が、その判定材料になる。
    math::Vector3 boundsExtents = {};

    // CPU 頂点配列からバウンディング球と AABB を計算する。
    // メッシュ生成後・GPU アップロード前に一度だけ呼ぶ想定。
    void ComputeBounds();

    // boundsExtents だけを CPU 頂点配列から埋める (球には触れない)。
    // WHY: .mesh / .fzasset は球だけをヘッダーに持つため、そちらから読んだメッシュは
    //      AABB を知らないまま「薄い形」と区別がつかなくなる。フォーマットを変えずに
    //      補うため、頂点を読み終えた直後にこれだけを走らせる。
    void ComputeBoundsExtents();
};

} // namespace fbzz::renderer
