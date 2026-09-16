/// @file    Print.hpp
/// @brief   数学型を GoogleTest の失敗メッセージで読める形に出す PrintTo。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// GoogleTest は自前の PrintTo が見つからない型をバイト列で出す。Vector3 の比較が
/// 落ちたときに «96-byte object <00-00 80-3F ...>» としか出ないのを避けるため、
/// ADL で拾われる fbzz::math 名前空間に出力関数を置く。
#pragma once

#include <iosfwd>

namespace fbzz::math {

struct Vector2;
struct Vector3;
struct Vector4;
struct Quaternion;
struct Matrix3;
struct Matrix4;

void PrintTo(const Vector2& v, std::ostream* os);
void PrintTo(const Vector3& v, std::ostream* os);
void PrintTo(const Vector4& v, std::ostream* os);
void PrintTo(const Quaternion& q, std::ostream* os);
void PrintTo(const Matrix3& m, std::ostream* os);
void PrintTo(const Matrix4& m, std::ostream* os);

} // namespace fbzz::math
