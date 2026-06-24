// FBZZ Engine
// Tests/TestHelper.hpp
// モジュールテスト共通ユーティリティ — check / checkF / checkV と結果カウンター
// 各テスト実行ファイルは 1 TU しか持たないため inline 変数で安全に共有できる。
#pragma once
#include <cstdio>
#include <cmath>

// テスト結果カウンター (実行ファイルにつき 1 インスタンス)
inline int g_passed = 0;
inline int g_failed = 0;

// 条件が真なら PASS、偽なら FAIL を出力して失敗数を増やす
inline void check(bool cond, const char* label, const char* detail = nullptr)
{
    if (cond)
    {
        std::printf("  [PASS] %s\n", label);
        ++g_passed;
    }
    else
    {
        if (detail)
            std::fprintf(stderr, "  [FAIL] %s  <- %s\n", label, detail);
        else
            std::fprintf(stderr, "  [FAIL] %s\n", label);
        ++g_failed;
    }
}

// float 値付き check ヘルパー
inline void checkF(bool cond, const char* label, float actual, const char* expr)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "actual=%.4f  (%s)", actual, expr);
    check(cond, label, cond ? nullptr : buf);
}

// Vector3 値付き check ヘルパー
inline void checkV(bool cond, const char* label,
                   float x, float y, float z, const char* expr)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "actual=(%.3f, %.3f, %.3f)  (%s)", x, y, z, expr);
    check(cond, label, cond ? nullptr : buf);
}
