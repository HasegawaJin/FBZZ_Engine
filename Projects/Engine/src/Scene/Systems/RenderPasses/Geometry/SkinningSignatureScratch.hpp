/// @file    SkinningSignatureScratch.hpp
/// @brief   Source-private, reentrant numeric signature scratch owned by one Engine DLL thread.
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

/// @note Numeric capacity survives until thread exit; words are empty between leases, and scene/resource/model references never enter the pool.
struct ScratchMetrics {
    size_t slots=0,pointerCapacity=0,activeLeases=0;
    uint64_t payloadBytes=0,storageBytes=0,pointerPayloadBytes=0;
};

/// @note Leases must end in stack order on their creating thread. Out-of-line acquisition shares production TLS with native tests.
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

/// @brief Read current-thread retained capacities; these gauges exclude allocator metadata and component-owned vectors.
FBZZ_SKINNING_SIGNATURE_SCRATCH_API ScratchMetrics CurrentSignatureScratchMetrics();

} /// @note namespace skinning_signature_detail
} /// @note namespace fbzz::scene
#undef FBZZ_SKINNING_SIGNATURE_SCRATCH_API