/// @file    StbImage.cpp
/// @brief   Editor 内で共有する stb_image 実装の唯一の翻訳単位。
/// @author  Hasegawa Jin
/// @date    2026-08-12

/// @note この翻訳単位だけが stb_image の実装を持つ。各利用側で個別に実装すると
///       未定義参照になり、複数生成はコンパイル時間とバイナリサイズも増える。
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
