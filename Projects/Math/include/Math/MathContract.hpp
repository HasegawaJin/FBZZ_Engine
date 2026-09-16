/// @file    MathContract.hpp
/// @brief   数学関数の契約違反を「落とさずに報告する」ための通報口。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
#pragma once
#include <atomic>

namespace fbzz::math {

struct ContractViolation {
    const char* expr     = nullptr;  // 破られた条件式
    const char* message  = nullptr;  // 何が起きたか / 代わりに返した値
    const char* function = nullptr;
    const char* file     = nullptr;
    int         line     = 0;
};

using ContractHandler = void (*)(const ContractViolation&);

/// 違反の報告先を差し替える。既定 (nullptr) では何もしない。
/// Engine 側が Logger へ流すハンドラーを起動時に 1 度だけ差す。
void SetContractHandler(ContractHandler handler);

/// FBZZ_MATH_CONTRACT から呼ぶ。alreadyReported が立っていれば何もしない。
/// WHY 1 度だけか: 違反は毎フレーム同じ場所から出る。抑止しないと Console が
///     1 件で埋まり、他のログが押し出されて «別の問題» が見えなくなる。
void ReportContract(const ContractViolation& violation, std::atomic<bool>& alreadyReported);

} // namespace fbzz::math

/// 契約違反を報告する (実行は止めない)。呼び出し側は直後に安全な値を返すこと。
/// 抑止フラグは展開ごとに別実体なので、報告は «呼び出し位置ごとに 1 度» になる。
#define FBZZ_MATH_CONTRACT(cond, msg)                                             \
    do {                                                                          \
        if (!(cond)) {                                                            \
            static std::atomic<bool> s_fbzzContractReported{ false };             \
            ::fbzz::math::ReportContract(                                         \
                ::fbzz::math::ContractViolation{ #cond, (msg), __func__,           \
                                                 __FILE__, __LINE__ },            \
                s_fbzzContractReported);                                          \
        }                                                                         \
    } while (false)
