/// @file    Measure.cpp
/// @brief   --measure の計時ループと JSON 出力。
/// @author  Hasegawa Jin
/// @date    2026-09-18
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

#include "Measure.hpp"

#include "MicroCases.hpp"
#include "Scenes/Scenes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef FBZZ_BENCH_CONFIG
    #define FBZZ_BENCH_CONFIG "unknown"
#endif

namespace fbzz::bench {

namespace {

/// @brief 1 項目ぶんの計時結果。
struct Result {
    const char*         kind;   ///< "scene" / "micro"
    std::string         name;
    const char*         phase;  ///< scene: "simulate" / "present"、micro: "op"
    const char*         unit;   ///< "us/step" / "ns/op"
    std::vector<double> samples;
};

/// @brief 正の整数を読む。
/// @return 読めなければ false。out は未変更。
bool ReadPositiveInt(const char* text, int& out)
{
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 100000000) return false;
    out = static_cast<int>(value);
    return true;
}

/// @brief 計時するスレッドを 1 コアへ固定し、優先度を上げる。
/// @note コア間の移動と他プロセスの割り込みが中央値の揺れの主因。前後比較で数 % の差を読むため先に揺れを削る。
/// @note コア 0 は割り込み処理が集まりやすいので、使えるなら 2 番目以降のコアを選ぶ。
void PinCurrentThread()
{
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask  = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || processMask == 0)
        return;

    DWORD_PTR chosen = 0;
    int       seen   = 0;
    for (int bit = 0; bit < static_cast<int>(sizeof(DWORD_PTR) * 8); ++bit) {
        const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << bit;
        if ((processMask & mask) == 0) continue;
        chosen = mask;
        if (++seen == 2) break;
    }
    SetThreadAffinityMask(GetCurrentThread(), chosen);
}

/// @brief CPU のブランド文字列を返す。
/// @see https://learn.microsoft.com/cpp/intrinsics/cpuid-cpuidex __cpuid (leaf 0x80000002〜0x80000004 が brand string)
std::string CpuBrand()
{
    std::array<int, 4> info{};
    __cpuid(info.data(), 0x80000000);
    if (static_cast<unsigned>(info[0]) < 0x80000004u) return "unknown";

    char brand[49] = {};
    for (int leaf = 0; leaf < 3; ++leaf) {
        __cpuid(info.data(), static_cast<int>(0x80000002u + leaf));
        std::memcpy(brand + leaf * 16, info.data(), 16);
    }
    std::string text = brand;
    const size_t first = text.find_first_not_of(' ');
    const size_t last  = text.find_last_not_of(' ');
    return first == std::string::npos ? "unknown" : text.substr(first, last - first + 1);
}

/// @brief _MSC_FULL_VER を "MSVC 19.51.36231" の形にする。
/// @see https://learn.microsoft.com/cpp/preprocessor/predefined-macros _MSC_FULL_VER
std::string CompilerVersion()
{
    char text[64] = {};
    std::snprintf(text, sizeof(text), "MSVC %d.%d.%d", _MSC_VER / 100, _MSC_VER % 100, _MSC_FULL_VER % 100000);
    return text;
}

/// @brief 現在時刻を UTC の ISO 8601 (秒まで) で返す。
std::string UtcNowIso8601()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

/// @brief JSON の文字列リテラルとして引用する。UTF-8 はそのまま通す。
/// @see https://www.rfc-editor.org/rfc/rfc8259 RFC 8259 Section 7 "Strings"
std::string QuoteJson(const std::string& text)
{
    std::string quoted = "\"";
    for (const char ch : text) {
        switch (ch) {
        case '"':  quoted += "\\\""; break;
        case '\\': quoted += "\\\\"; break;
        case '\n': quoted += "\\n";  break;
        case '\r': quoted += "\\r";  break;
        case '\t': quoted += "\\t";  break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char escaped[8] = {};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(ch));
                quoted += escaped;
            } else {
                quoted += ch;
            }
        }
    }
    quoted += '"';
    return quoted;
}

double Median(std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void PrintResult(const Result& result)
{
    const auto [minIt, maxIt] = std::minmax_element(result.samples.begin(), result.samples.end());
    std::printf("%-6s %-9s %12.3f %12.3f %12.3f  %-8s %s\n", result.kind, result.phase,
                Median(result.samples), *minIt, *maxIt, result.unit, result.name.c_str());
    std::fflush(stdout);
}

bool WriteJson(const MeasureOptions& options, const std::string& startedAt, const std::vector<Result>& results)
{
    std::FILE* file = nullptr;
    if (fopen_s(&file, options.jsonPath.c_str(), "wb") != 0 || file == nullptr) return false;

    std::fprintf(file, "{\n  \"schema\": \"fbzz-bench/1\",\n");
    std::fprintf(file, "  \"label\": %s,\n", QuoteJson(options.label).c_str());
    std::fprintf(file, "  \"startedAt\": %s,\n", QuoteJson(startedAt).c_str());
    std::fprintf(file, "  \"environment\": {\"commit\": %s, \"dirty\": %s, \"cpu\": %s, \"logicalCores\": %lu, "
                       "\"compiler\": %s, \"config\": %s},\n",
                 QuoteJson(options.commit.empty() ? "unknown" : options.commit).c_str(),
                 options.dirty ? "true" : "false", QuoteJson(CpuBrand()).c_str(),
                 GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), QuoteJson(CompilerVersion()).c_str(),
                 QuoteJson(FBZZ_BENCH_CONFIG).c_str());
    std::fprintf(file, "  \"settings\": {\"steps\": %d, \"warmup\": %d, \"repeats\": %d, \"ops\": %d, \"fixedStep\": %.7f},\n",
                 options.steps, options.warmupSteps, options.repeats, options.ops, kBenchFixedStep);
    std::fprintf(file, "  \"results\": [\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const Result& result = results[i];
        const auto [minIt, maxIt] = std::minmax_element(result.samples.begin(), result.samples.end());
        std::fprintf(file, "    {\"kind\": \"%s\", \"name\": %s, \"phase\": \"%s\", \"unit\": \"%s\", \"samples\": [",
                     result.kind, QuoteJson(result.name).c_str(), result.phase, result.unit);
        for (size_t s = 0; s < result.samples.size(); ++s)
            std::fprintf(file, "%s%.4f", s == 0 ? "" : ", ", result.samples[s]);
        std::fprintf(file, "], \"median\": %.4f, \"min\": %.4f, \"max\": %.4f}%s\n",
                     Median(result.samples), *minIt, *maxIt, i + 1 < results.size() ? "," : "");
    }
    std::fprintf(file, "  ]\n}\n");
    return std::fclose(file) == 0;
}

} // namespace

bool ParseMeasureOptions(int argc, char** argv, MeasureOptions& out)
{
    bool           found = false;
    MeasureOptions parsed;
    for (int i = 1; i < argc; ++i) {
        const char* arg  = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (std::strcmp(arg, "--measure") == 0) {
            found = true;
        } else if (std::strcmp(arg, "--dirty") == 0) {
            parsed.dirty = true;
        } else if (next != nullptr && std::strcmp(arg, "--steps") == 0) {
            if (!ReadPositiveInt(next, parsed.steps)) parsed.steps = -1;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--warmup") == 0) {
            if (!ReadPositiveInt(next, parsed.warmupSteps)) parsed.warmupSteps = -1;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--repeats") == 0) {
            if (!ReadPositiveInt(next, parsed.repeats)) parsed.repeats = -1;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--ops") == 0) {
            if (!ReadPositiveInt(next, parsed.ops)) parsed.ops = -1;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--json") == 0) {
            parsed.jsonPath = next;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--label") == 0) {
            parsed.label = next;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--commit") == 0) {
            parsed.commit = next;
            ++i;
        }
    }
    if (found) out = parsed;
    return found;
}

int RunMeasure(const MeasureOptions& options)
{
    SetConsoleOutputCP(CP_UTF8);

    if (options.steps <= 0 || options.warmupSteps <= 0 || options.repeats <= 0 || options.ops <= 0) {
        std::fprintf(stderr, "--steps / --warmup / --repeats / --ops は 1 以上の整数で指定してください。\n");
        return 1;
    }

    const std::string startedAt = UtcNowIso8601();
    PinCurrentThread();

    std::printf("steps=%d warmup=%d repeats=%d ops=%d label=%s\n", options.steps, options.warmupSteps,
                options.repeats, options.ops, options.label.empty() ? "-" : options.label.c_str());
    std::printf("%-6s %-9s %12s %12s %12s  %-8s %s\n", "kind", "phase", "median", "min", "max", "unit", "name");

    const auto elapsed = [](auto begin, auto end) {
        return std::chrono::duration<double, std::micro>(end - begin).count();
    };

    std::vector<Result> results;

    for (const std::unique_ptr<BenchScene>& scene : MakeAllScenes()) {
        Result simulate{ "scene", scene->Name(), "simulate", "us/step", {} };
        Result present { "scene", scene->Name(), "present",  "us/step", {} };
        for (int r = 0; r < options.repeats; ++r) {
            scene->Reset();
            for (int s = 0; s < options.warmupSteps; ++s) {
                scene->Simulate(kBenchFixedStep);
                scene->Present();
            }

            const auto simBegin = std::chrono::steady_clock::now();
            for (int s = 0; s < options.steps; ++s)
                scene->Simulate(kBenchFixedStep);
            const auto simEnd = std::chrono::steady_clock::now();
            simulate.samples.push_back(elapsed(simBegin, simEnd) / options.steps);

            /// @note 問い合わせ系の場面 (BVH / CCD / NarrowPhase) は仕事の本体が Present にある。止めた状態で同じ回数だけ回して別に計る。
            const auto presentBegin = std::chrono::steady_clock::now();
            for (int s = 0; s < options.steps; ++s)
                scene->Present();
            const auto presentEnd = std::chrono::steady_clock::now();
            present.samples.push_back(elapsed(presentBegin, presentEnd) / options.steps);
        }
        PrintResult(simulate);
        PrintResult(present);
        results.push_back(std::move(simulate));
        results.push_back(std::move(present));
    }

    /// @note 畳んだ値を volatile へ書き、演算ごと最適化で消されるのを防ぐ。
    volatile double sink = 0.0;
    for (const MicroCase& micro : AllMicroCases()) {
        Result result{ "micro", micro.name, "op", "ns/op", {} };
        sink = sink + micro.run(std::max(1, options.ops / 10));
        for (int r = 0; r < options.repeats; ++r) {
            const auto begin = std::chrono::steady_clock::now();
            sink = sink + micro.run(options.ops);
            const auto end = std::chrono::steady_clock::now();
            result.samples.push_back(elapsed(begin, end) * 1000.0 / options.ops);
        }
        PrintResult(result);
        results.push_back(std::move(result));
    }

    if (!options.jsonPath.empty() && !WriteJson(options, startedAt, results)) {
        std::fprintf(stderr, "JSON を書けません: %s\n", options.jsonPath.c_str());
        return 1;
    }
    return 0;
}

} // namespace fbzz::bench
