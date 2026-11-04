/// @file    Api.hpp
/// @brief   Core DLL の明示的な公開境界。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#if defined(FBZZCore_EXPORTS)
#define FBZZ_CORE_API __declspec(dllexport)
#else
#define FBZZ_CORE_API __declspec(dllimport)
#endif
