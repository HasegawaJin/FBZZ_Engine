/// @file    Measure.cpp
/// @brief   --measure の計時ループと結果出力。
/// @author  Hasegawa Jin
/// @date    2026-09-18
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "Measure.hpp"

#include "Scenes/Scenes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

namespace fbzz::bench {

namespace {

/// @brief "--name value" 形式の正の整数を読む。
/// @return 読めなければ false。out は未変更。
bool ReadPositiveInt(const char* text, int& out)
{
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 1000000) return false;
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

/// @brief CSV の 1 フィールドを RFC 4180 の規則で引用する。
/// @see https://www.rfc-editor.org/rfc/rfc4180 RFC 4180 Section 2 "Definition of the CSV Format"
std::string QuoteCsv(const char* text)
{
    std::string quoted = "\"";
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '"') quoted += '"';
        quoted += *p;
    }
    quoted += '"';
    return quoted;
}

} // namespace

bool ParseMeasureOptions(int argc, char** argv, MeasureOptions& out)
{
    bool          found = false;
    MeasureOptions parsed;
    for (int i = 1; i < argc; ++i) {
        const char* arg  = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (std::strcmp(arg, "--measure") == 0) {
            found = true;
        } else if (next != nullptr && std::strcmp(arg, "--steps") == 0) {
            if (!ReadPositiveInt(next, parsed.steps)) parsed.steps = -1;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--warmup") == 0) {
            if (!ReadPositiveInt(next, parsed.warmupSteps)) parsed.warmupSteps = -1;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--repeats") == 0) {
            if (!ReadPositiveInt(next, parsed.repeats)) parsed.repeats = -1;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--csv") == 0) {
            parsed.csvPath = next;
            ++i;
        } else if (next != nullptr && std::strcmp(arg, "--label") == 0) {
            parsed.label = next;
            ++i;
        }
    }
    if (found) out = parsed;
    return found;
}

int RunMeasure(const MeasureOptions& options)
{
    SetConsoleOutputCP(CP_UTF8);

    if (options.steps <= 0 || options.warmupSteps <= 0 || options.repeats <= 0) {
        std::fprintf(stderr, "--steps / --warmup / --repeats は 1 以上の整数で指定してください。\n");
        return 1;
    }

    std::FILE* csv = nullptr;
    if (!options.csvPath.empty()) {
        if (fopen_s(&csv, options.csvPath.c_str(), "ab") != 0 || csv == nullptr) {
            std::fprintf(stderr, "CSV を開けません: %s\n", options.csvPath.c_str());
            return 1;
        }
        /// @note 追記先が空のときだけ見出し行を書く。before / after を同じファイルへ積むため。
        std::fseek(csv, 0, SEEK_END);
        if (std::ftell(csv) == 0)
            std::fprintf(csv, "label,scene,phase,median_us,min_us,max_us,steps,warmup,repeats\n");
    }

    PinCurrentThread();

    std::printf("steps=%d warmup=%d repeats=%d label=%s\n",
                options.steps, options.warmupSteps, options.repeats,
                options.label.empty() ? "-" : options.label.c_str());
    std::printf("%-9s %12s %12s %12s  %s\n", "phase", "median[us]", "min[us]", "max[us]", "scene");

    std::vector<std::unique_ptr<BenchScene>> scenes = MakeAllScenes();
    std::vector<double> simulateUs;
    std::vector<double> presentUs;
    simulateUs.reserve(static_cast<size_t>(options.repeats));
    presentUs.reserve(static_cast<size_t>(options.repeats));

    const auto elapsedUs = [](auto begin, auto end) {
        return std::chrono::duration<double, std::micro>(end - begin).count();
    };

    for (const std::unique_ptr<BenchScene>& scene : scenes) {
        simulateUs.clear();
        presentUs.clear();
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
            simulateUs.push_back(elapsedUs(simBegin, simEnd) / options.steps);

            /// @note 問い合わせ系の場面 (BVH / CCD / NarrowPhase) は仕事の本体が Present にある。止めた状態で同じ回数だけ回して別に計る。
            const auto presentBegin = std::chrono::steady_clock::now();
            for (int s = 0; s < options.steps; ++s)
                scene->Present();
            const auto presentEnd = std::chrono::steady_clock::now();
            presentUs.push_back(elapsedUs(presentBegin, presentEnd) / options.steps);
        }

        for (auto [phase, samples] : { std::pair<const char*, std::vector<double>*>{ "simulate", &simulateUs },
                                       std::pair<const char*, std::vector<double>*>{ "present",  &presentUs } }) {
            std::sort(samples->begin(), samples->end());
            const double median = (*samples)[samples->size() / 2];
            const double minUs  = samples->front();
            const double maxUs  = samples->back();

            std::printf("%-9s %12.2f %12.2f %12.2f  %s\n", phase, median, minUs, maxUs, scene->Name());

            if (csv != nullptr) {
                std::fprintf(csv, "%s,%s,%s,%.3f,%.3f,%.3f,%d,%d,%d\n",
                             QuoteCsv(options.label.c_str()).c_str(), QuoteCsv(scene->Name()).c_str(), phase,
                             median, minUs, maxUs, options.steps, options.warmupSteps, options.repeats);
            }
        }
        std::fflush(stdout);
    }

    if (csv != nullptr) std::fclose(csv);
    return 0;
}

} // namespace fbzz::bench
