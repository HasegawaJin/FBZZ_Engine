/// @file    DebugDrawBatch.hpp
/// @brief   デバッグ描画バッチへの追記規則 (満杯なら先に吐き出す)。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#pragma once

#include <cstddef>
#include <vector>

namespace fbzz::renderer {

/// @brief プリミティブ 1 個分の頂点をバッチへ積む。入り切らなければ先に flush を呼ぶ。
/// @param capacity  バッチ 1 本の頂点上限。
/// @param unbounded true なら上限を見ない (記録中で GPU へ出さない場合)。
/// @param flush     バッチを空にする呼び出し。空にならなければ積まずに false を返す。
/// @return 積めたら true。
/// @note プリミティブは分割しない。LINE_LIST / TRIANGLE_LIST の組が Flush を跨ぐと形が崩れる。
template<typename Vertex, typename FlushFn>
bool AppendDebugPrimitive(std::vector<Vertex>& batch, std::size_t capacity,
                          const Vertex* vertices, std::size_t count,
                          bool unbounded, FlushFn&& flush)
{
    if (!unbounded) {
        if (count > capacity) return false;
        if (batch.size() + count > capacity) flush();
        if (batch.size() + count > capacity) return false;
    }
    batch.insert(batch.end(), vertices, vertices + count);
    return true;
}

} /// @note namespace fbzz::renderer
