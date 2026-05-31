// FBZZ Engine
// Uuid.hpp | fbzz::util
// UUID v4 (ランダム) 生成ユーティリティ
#pragma once
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace fbzz::util {

// UUID v4 を "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" 形式で生成する。
// WHY: GameObject の名前はユーザーが変更できるため、シリアライズ時の
//      クロスオブジェクト参照 (IK Pole/Target 等) に名前を使うと
//      リネーム後に参照が壊れる。永続的な UUID を識別子とすることで耐性を持たせる。
inline std::string GenerateUUID()
{
    // random_device でシードし mt19937_64 で 128 ビット取得する。
    // WHY: 同フレームで複数 UUID を生成するため、コスト高な random_device を
    //      シード用にのみ使い、その後の乱数は mt19937_64 に委ねる。
    static thread_local std::mt19937_64 s_gen{ std::random_device{}() };
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t hi = dis(s_gen);
    uint64_t lo = dis(s_gen);

    // Version 4 (ランダム): time_hi_and_version の上位 4 ビットを 0100 に設定
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Variant 1 (RFC 4122): clock_seq_hi_and_reserved の上位 2 ビットを 10 に設定
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(hi >> 32),
        static_cast<uint32_t>((hi >> 16) & 0xFFFFU),
        static_cast<uint32_t>(hi         & 0xFFFFU),
        static_cast<uint32_t>(lo >> 48),
        static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFULL));
    return buf;
}

} // namespace fbzz::util
