/// @file    MathContract.hpp
/// @brief   数学関数の契約違反を「落とさずに報告する」ための通報口。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
#pragma once
#include <atomic>

namespace fbzz::math {

struct ContractViolation {
    const char* expr     = nullptr;  ///< 破られた条件式。
    const char* message  = nullptr;  ///< 何が起きたか / 代わりに返した値。
    const char* function = nullptr;
    const char* file     = nullptr;
    int         line     = 0;
};

using ContractHandler = void (*)(const ContractViolation&);

/// @brief 違反の報告先を差し替える。
/// @note 既定 (nullptr) は何もしない。Engine が起動時に Logger 連携ハンドラーを 1 度だけ差す。
void SetContractHandler(ContractHandler handler);

/// @brief 契約違反を報告する。FBZZ_MATH_CONTRACT から呼ぶ。
/// @note alreadyReported が立っていれば何もしない。同じ箇所からの連続報告で Console が埋まるのを防ぐ。
void ReportContract(const ContractViolation& violation, std::atomic<bool>& alreadyReported);

} // namespace fbzz::math

/// @brief 契約違反を報告する (実行は止めない)。呼び出し側は直後に安全な値を返すこと。
/// @note 抑止フラグは展開ごとに別実体になるため、報告は呼び出し位置ごとに 1 度になる。
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
