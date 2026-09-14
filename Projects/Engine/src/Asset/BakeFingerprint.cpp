/// @file    BakeFingerprint.cpp
/// @brief   焼いた出力の指紋の実装
/// @author  Hasegawa Jin
/// @date    2026-09-14
#include <Engine/Asset/BakeFingerprint.hpp>

#include <bit>
#include <cmath>

namespace fbzz::asset {
namespace {

constexpr std::uint64_t kPrime = 1099511628211ull;

std::uint64_t MixByte(std::uint64_t hash, std::uint8_t byte)
{
    return (hash ^ byte) * kPrime;
}

} // namespace

void BakeFingerprint::Add(std::span<const std::uint8_t> bytes)
{
    for (const std::uint8_t byte : bytes) m_hash = MixByte(m_hash, byte);
}

void BakeFingerprint::Add(std::span<const float> values)
{
    for (const float value : values) {
        const float normalized = std::isnan(value) ? 0.0f : (value == 0.0f ? 0.0f : value);
        Add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(normalized)));
    }
}

void BakeFingerprint::Add(std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        m_hash = MixByte(m_hash, static_cast<std::uint8_t>((value >> shift) & 0xffu));
}

void BakeFingerprint::Add(std::string_view text)
{
    for (const char c : text) m_hash = MixByte(m_hash, static_cast<std::uint8_t>(c));
}

std::string BakeFingerprint::Finish() const
{
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    std::uint64_t value = m_hash;
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xfull];
        value >>= 4;
    }
    return out;
}

std::string BakeFingerprintOf(std::span<const std::uint8_t> bytes)
{
    BakeFingerprint fingerprint;
    fingerprint.Add(bytes);
    return fingerprint.Finish();
}

std::string BakeFingerprintOf(std::span<const float> values)
{
    BakeFingerprint fingerprint;
    fingerprint.Add(values);
    return fingerprint.Finish();
}

} // namespace fbzz::asset
