//===--- ConstraintSystemTests.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/ConstraintSystem.h"
#include "llvm/ADT/STLExtras.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

using RowTy = ConstraintSystem::RowTy;

/// Convert the dense coefficient vector \p R, indexed by variable with the
/// constant part at index 0, to a row.
static RowTy toRow(ArrayRef<int64_t> R) {
  RowTy Row;
  Row.emplace_back(R[0], 0);
  for (auto [Idx, C] : enumerate(R.drop_front()))
    if (C != 0)
      Row.emplace_back(C, Idx + 1);
  return Row;
}

/// Add the dense coefficient vector \p R to \p CS.
static void addVariableRow(ConstraintSystem &CS, ArrayRef<int64_t> R) {
  CS.addRow(toRow(R), R.size() - 1);
}

/// Returns true if the condition described by the dense coefficient vector \p R
/// is implied by \p CS.
static bool isConditionImplied(const ConstraintSystem &CS,
                               ArrayRef<int64_t> R) {
  return CS.isConditionImplied(toRow(R));
}

TEST(ConstraintSolverTest, TestSolutionChecks) {
  {
    ConstraintSystem CS;
    // x + y <= 10, x >= 5, y >= 6, x <= 10, y <= 10
    addVariableRow(CS, {10, 1, 1});
    addVariableRow(CS, {-5, -1, 0});
    addVariableRow(CS, {-6, 0, -1});
    addVariableRow(CS, {10, 1, 0});
    addVariableRow(CS, {10, 0, 1});

    EXPECT_FALSE(CS.mayHaveSolution());
  }

  {
    ConstraintSystem CS;
    // x + y <= 10, x >= 2, y >= 3, x <= 10, y <= 10
    addVariableRow(CS, {10, 1, 1});
    addVariableRow(CS, {-2, -1, 0});
    addVariableRow(CS, {-3, 0, -1});
    addVariableRow(CS, {10, 1, 0});
    addVariableRow(CS, {10, 0, 1});

    EXPECT_TRUE(CS.mayHaveSolution());
  }

  {
    ConstraintSystem CS;
    // x + y <= 10, x >= 10, y >= 10; does not have a solution.
    addVariableRow(CS, {10, 1, 1});
    addVariableRow(CS, {-10, -1, 0});
    addVariableRow(CS, {-10, 0, -1});

    EXPECT_FALSE(CS.mayHaveSolution());
  }

  {
    ConstraintSystem CS;
    // x + y >= 20, 10 >= x, 10 >= y; does HAVE a solution.
    addVariableRow(CS, {-20, -1, -1});
    addVariableRow(CS, {-10, -1, 0});
    addVariableRow(CS, {-10, 0, -1});

    EXPECT_TRUE(CS.mayHaveSolution());
  }

  {
    ConstraintSystem CS;

    // 2x + y + 3z <= 10,  2x + y >= 10, y >= 1
    addVariableRow(CS, {10, 2, 1, 3});
    addVariableRow(CS, {-10, -2, -1, 0});
    addVariableRow(CS, {-1, 0, 0, -1});

    EXPECT_FALSE(CS.mayHaveSolution());
  }

  {
    ConstraintSystem CS;

    // 2x + y + 3z <= 10,  2x + y >= 10
    addVariableRow(CS, {10, 2, 1, 3});
    addVariableRow(CS, {-10, -2, -1, 0});

    EXPECT_TRUE(CS.mayHaveSolution());
  }
}

TEST(ConstraintSolverTest, IsConditionImplied) {
  {
    // For the test below, we assume we know
    // x <= 5 && y <= 3
    ConstraintSystem CS;
    addVariableRow(CS, {5, 1, 0});
    addVariableRow(CS, {3, 0, 1});

    // x + y <= 6 does not hold.
    EXPECT_FALSE(isConditionImplied(CS, {6, 1, 1}));
    // x + y <= 7 does not hold.
    EXPECT_FALSE(isConditionImplied(CS, {7, 1, 1}));
    // x + y <= 8 does hold.
    EXPECT_TRUE(isConditionImplied(CS, {8, 1, 1}));

    // 2 * x + y <= 12 does hold.
    EXPECT_FALSE(isConditionImplied(CS, {12, 2, 1}));
    // 2 * x + y <= 13 does hold.
    EXPECT_TRUE(isConditionImplied(CS, {13, 2, 1}));

    //  x + y <= 12 does hold.
    EXPECT_FALSE(isConditionImplied(CS, {12, 2, 1}));
    // 2 * x + y <= 13 does hold.
    EXPECT_TRUE(isConditionImplied(CS, {13, 2, 1}));

    // x <= y == x - y <= 0 does not hold.
    EXPECT_FALSE(isConditionImplied(CS, {0, 1, -1}));
    // y <= x == -x + y <= 0 does not hold.
    EXPECT_FALSE(isConditionImplied(CS, {0, -1, 1}));
  }

  {
    // For the test below, we assume we know
    // x + 1 <= y + 1 == x - y <= 0
    ConstraintSystem CS;
    addVariableRow(CS, {0, 1, -1});

    // x <= y == x - y <= 0 does hold.
    EXPECT_TRUE(isConditionImplied(CS, {0, 1, -1}));
    // y <= x == -x + y <= 0 does not hold.
    EXPECT_FALSE(isConditionImplied(CS, {0, -1, 1}));

    // x <= y + 10 == x - y <= 10 does hold.
    EXPECT_TRUE(isConditionImplied(CS, {10, 1, -1}));
    // x + 10 <= y == x - y <= -10 does NOT hold.
    EXPECT_FALSE(isConditionImplied(CS, {-10, 1, -1}));
  }

  {
    // For the test below, we assume we know
    // x <= y == x - y <= 0
    // y <= z == y - x <= 0
    ConstraintSystem CS;
    addVariableRow(CS, {0, 1, -1, 0});
    addVariableRow(CS, {0, 0, 1, -1});

    // z <= y == -y + z <= 0 does not hold.
    EXPECT_FALSE(isConditionImplied(CS, {0, 0, -1, 1}));
    // x <= z == x - z <= 0 does hold.
    EXPECT_TRUE(isConditionImplied(CS, {0, 1, 0, -1}));
  }
}

TEST(ConstraintSolverTest, IsConditionImpliedOverflow) {
  ConstraintSystem CS;
  // Make sure isConditionImplied returns false when there is an overflow.
  int64_t Limit = std::numeric_limits<int64_t>::max();
  addVariableRow(CS, {Limit - 1, Limit - 2, Limit - 3});
  EXPECT_FALSE(isConditionImplied(CS, {Limit - 1, Limit - 2, Limit - 3}));
}

TEST(ConstraintSolverTest, LargeVariableIndex) {
  // A variable index that does not fit in 16 bits must not be truncated, as
  // that would record the constraint against an unrelated variable. Such
  // indices are reachable in practice, e.g. for a function with more than
  // 65535 arguments.
  constexpr unsigned BigIdx = 65537;
  // The index BigIdx aliases when truncated to 16 bits.
  constexpr unsigned AliasIdx = BigIdx & 0xffff;
  static_assert(AliasIdx == 1, "expected BigIdx to alias variable 1");

  // Build the rows directly rather than via addVariableRow, to avoid dense
  // coefficient vectors with 65538 entries.
  ConstraintSystem CS;
  // v[BigIdx] <= 4
  RowTy Row = {{4, 0}, {1, BigIdx}};
  ASSERT_TRUE(CS.addRow(Row, BigIdx));

  // The coefficient must be recorded for BigIdx, not for AliasIdx.
  const RowTy &Last = CS.getLastConstraint();
  ASSERT_EQ(Last.size(), 2u);
  EXPECT_EQ(Last[1].Id, BigIdx);
  EXPECT_EQ(Last[1].Coefficient, 1);

  // The system does not constrain v[AliasIdx], so v[AliasIdx] <= 4 must not be
  // implied.
  RowTy AliasQuery = {{4, 0}, {1, AliasIdx}};
  EXPECT_FALSE(CS.isConditionImplied(AliasQuery));
  EXPECT_FALSE(CS.isConditionImpliedInSubSystem(AliasQuery));

  // v[BigIdx] <= 4 is implied, v[BigIdx] <= 3 is not.
  EXPECT_TRUE(CS.isConditionImplied(Row));
  EXPECT_TRUE(CS.isConditionImpliedInSubSystem(Row));
  Row[0].Coefficient = 3;
  EXPECT_FALSE(CS.isConditionImplied(Row));
  EXPECT_FALSE(CS.isConditionImpliedInSubSystem(Row));
}
} // namespace
