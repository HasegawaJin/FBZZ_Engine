/// @file    TestKit.hpp
/// @brief   TestKit の一括インクルード。Math / Physics のテストはこれ 1 本で足りる。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

#include <TestKit/Approx.hpp>
#include <TestKit/Deterministic.hpp>
#include <TestKit/Fixture.hpp>
#include <TestKit/Print.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/Tolerance.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
