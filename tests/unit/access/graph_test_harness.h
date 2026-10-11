#pragma once

// Maps the Graph A11y kernel conformance suite's harness macros onto doctest, so the suite
// transcribes from upstream unchanged apart from its include lines.

#include "doctest/doctest.h"

#define TEST(name) TEST_CASE(#name)

#undef CHECK_THROWS
#define CHECK_THROWS(ExType, expr) CHECK_THROWS_AS((void)(expr), ExType)
