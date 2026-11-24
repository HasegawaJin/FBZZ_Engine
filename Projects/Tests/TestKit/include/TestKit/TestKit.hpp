/// @file    TestKit.hpp
/// @brief   TestKit の一括インクルード。Math / Physics のテストはこれ 1 本で足りる。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

#include <TestKit/Assertions/Approx.hpp>
#include <TestKit/Support/Deterministic.hpp>
#include <TestKit/Fixture.hpp>
#include <TestKit/Output/Print.hpp>
#include <TestKit/Support/TempDir.hpp>
#include <TestKit/Assertions/Tolerance.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
