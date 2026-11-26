/// @file    SkinningSignatureScratch.cpp
/// @brief   スキニング入力の数値作業領域を再入可能なスレッド別プールから借用する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "SkinningSignatureScratch.hpp"
#include "../../../Scene/Systems/Animation/AnimationWorkTrace.hpp"

#include <memory>
#include <utility>

namespace fbzz::scene::skinning_signature_detail {

namespace {

/// @note プールの所有ポインター配列が再確保されても、入れ子で借りた数値バッファ自身は移動しない。
struct SignatureScratchStorage {std::vector<uint32_t> words;};
struct SignatureScratchPool {
    std::vector<std::unique_ptr<SignatureScratchStorage>> entries;
    size_t depth=0;
};
SignatureScratchPool& SignaturePool()
{
    thread_local SignatureScratchPool pool;
    return pool;
}

}

SignatureScratchLease::SignatureScratchLease(AnimationSkinningCpuRow* trace)
{
    auto& pool=SignaturePool();
    if(pool.depth==pool.entries.size()){
        const size_t pointerCapacityBefore=pool.entries.capacity();
        auto storage=std::make_unique<SignatureScratchStorage>();
        pool.entries.push_back(std::move(storage));
        if(trace){
            ++trace->signatureScratchStorageAllocationCalls;
            trace->signatureScratchStorageAllocationBytes+=sizeof(SignatureScratchStorage);
            if(pool.entries.capacity()!=pointerCapacityBefore){
                ++trace->signatureScratchPoolPointerAllocationCalls;
                trace->signatureScratchPoolPointerAllocationBytes+=pool.entries.capacity()*sizeof(std::unique_ptr<SignatureScratchStorage>);
            }
        }
    }
    m_words=&pool.entries[pool.depth]->words;
    m_words->clear();
    ++pool.depth;
}

SignatureScratchLease::~SignatureScratchLease()
{
    m_words->clear();
    --SignaturePool().depth;
}

ScratchMetrics CurrentSignatureScratchMetrics()
{
    const auto& pool=SignaturePool();
    ScratchMetrics metrics;
    metrics.slots=pool.entries.size();
    metrics.pointerCapacity=pool.entries.capacity();
    metrics.activeLeases=pool.depth;
    metrics.storageBytes=pool.entries.size()*sizeof(SignatureScratchStorage);
    metrics.pointerPayloadBytes=pool.entries.capacity()*sizeof(std::unique_ptr<SignatureScratchStorage>);
    for(const auto& entry:pool.entries) {
        metrics.payloadBytes+=entry->words.capacity()*sizeof(uint32_t);
    }
    return metrics;
}

}
