/// @file    SkinningSignatureScratch.hpp
/// @brief   Engine DLL のスレッドが所有する、再入可能な内部数値シグネチャ作業領域。
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(_WIN32) && defined(FBZZEngine_EXPORTS)
#define FBZZ_SKINNING_SIGNATURE_SCRATCH_API __declspec(dllexport)
#elif defined(_WIN32)
#define FBZZ_SKINNING_SIGNATURE_SCRATCH_API __declspec(dllimport)
#else
#define FBZZ_SKINNING_SIGNATURE_SCRATCH_API
#endif

namespace fbzz::scene {
struct AnimationSkinningCpuRow;
namespace skinning_signature_detail {

/// @note 数値の容量だけをスレッド終了まで保持し、借用の間は空にしてシーン・資源・モデルの参照を入れない。
struct ScratchMetrics {
    size_t slots=0,pointerCapacity=0,activeLeases=0;
    uint64_t payloadBytes=0,storageBytes=0,pointerPayloadBytes=0;
};

/// @note 借用元スレッドで取得の逆順に返却すること。取得の定義を共通化し本体とテストで同じスレッドローカル状態を使う。
class SignatureScratchLease {
public:
    FBZZ_SKINNING_SIGNATURE_SCRATCH_API explicit SignatureScratchLease(AnimationSkinningCpuRow* trace=nullptr);
    FBZZ_SKINNING_SIGNATURE_SCRATCH_API ~SignatureScratchLease();
    std::vector<uint32_t>& Words() noexcept {return *m_words;}
    SignatureScratchLease(const SignatureScratchLease&)=delete;
    SignatureScratchLease& operator=(const SignatureScratchLease&)=delete;
    SignatureScratchLease(SignatureScratchLease&&)=delete;
    SignatureScratchLease& operator=(SignatureScratchLease&&)=delete;
private:
    std::vector<uint32_t>* m_words=nullptr;
};

/// @brief 現在のスレッドで保持する容量を返す。アロケーターの管理情報とコンポーネント所有の配列を除く。
FBZZ_SKINNING_SIGNATURE_SCRATCH_API ScratchMetrics CurrentSignatureScratchMetrics();

} // namespace skinning_signature_detail
} // namespace fbzz::scene
#undef FBZZ_SKINNING_SIGNATURE_SCRATCH_API