//=== llvm/unittest/ADT/DepthFirstIteratorTest.cpp - DFS iterator tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DepthFirstIterator.h"
#include "TestGraph.h"
#include "gtest/gtest.h"

#include <array>
#include <iterator>
#include <type_traits>

#include <cstddef>

using namespace llvm;

namespace llvm {

template <typename T> struct CountedSet {
  using iterator = typename SmallPtrSet<T, 4>::iterator;

  SmallPtrSet<T, 4> S;
  int InsertVisited = 0;

  std::pair<iterator, bool> insert(const T &Item) {
    InsertVisited++;
    return S.insert(Item);
  }

  size_t count(const T &Item) const { return S.count(Item); }
  
  void completed(T) { }
};

template <typename T> class df_iterator_storage<CountedSet<T>, true> {
public:
  df_iterator_storage(CountedSet<T> &VSet) : Visited(VSet) {}

  CountedSet<T> &Visited;
};

TEST(DepthFirstIteratorTest, ActuallyUpdateIterator) {
  using StorageT = CountedSet<Graph<3>::NodeType *>;
  using DFIter = df_iterator<Graph<3>, StorageT, true>;

  Graph<3> G;
  G.AddEdge(0, 1);
  G.AddEdge(0, 2);
  StorageT S;
  for (auto N : make_range(DFIter::begin(G, S), DFIter::end(G, S)))
    (void)N;

  EXPECT_EQ(3, S.InsertVisited);
}

static_assert(
    std::is_convertible_v<decltype(*std::declval<df_iterator<Graph<3>>>()),
                          df_iterator<Graph<3>>::reference>);

// df_iterator should be (at-least) a forward-iterator
static_assert(std::is_base_of_v<std::forward_iterator_tag,
                                df_iterator<Graph<4>>::iterator_category>);

// df_ext_iterator cannot provide multi-pass guarantee, therefore its only
// an input-iterator
static_assert(std::is_same_v<df_ext_iterator<Graph<4>>::iterator_category,
                             std::input_iterator_tag>);

TEST(DepthFirstIteratorTest, MultiPassSafeWithInternalSet) {
  Graph<4> G;
  G.AddEdge(0, 1);
  G.AddEdge(1, 2);
  G.AddEdge(1, 3);

  std::array<decltype(G)::NodeType *, 4> NodesFirstPass, NodesSecondPass;

  auto B = df_begin(G), E = df_end(G);

  std::size_t I = 0;
  for (auto It = B; It != E; ++It)
    NodesFirstPass[I++] = *It;

  I = 0;
  for (auto It = B; It != E; ++It)
    NodesSecondPass[I++] = *It;

  EXPECT_EQ(NodesFirstPass, NodesSecondPass);
}

using NodeT = Graph<5>::NodeType;

// Graph with DFS order 0, 1, 3, 2. Node 4 is unreachable.
class DepthFirstIteratorGraphTest : public testing::Test {
protected:
  Graph<5> G;
  NodeT *N0 = G.AccessNode(0), *N1 = G.AccessNode(1), *N2 = G.AccessNode(2),
        *N3 = G.AccessNode(3), *N4 = G.AccessNode(4);

  DepthFirstIteratorGraphTest() {
    G.AddEdge(0, 1);
    G.AddEdge(0, 2);
    G.AddEdge(1, 3);
    G.AddEdge(3, 0);
    G.AddEdge(4, 0);
  }
};

TEST_F(DepthFirstIteratorGraphTest, CopiesAreIndependent) {
  // Copies of an iterator at the root.
  auto B = df_begin(G), E = df_end(G);
  auto It = B;
  EXPECT_EQ(It, B);
  EXPECT_EQ(*++It, N1);
  EXPECT_NE(It, B);
  EXPECT_EQ(*B, N0);
  EXPECT_EQ(B, df_begin(G));

  // Copies of an iterator during the traversal.
  auto Copy = It;
  EXPECT_EQ(Copy, It);
  EXPECT_EQ(*++Copy, N3);
  EXPECT_EQ(*++Copy, N2);
  EXPECT_EQ(*It, N1);
  EXPECT_FALSE(It.nodeVisited(N2));
  EXPECT_TRUE(Copy.nodeVisited(N2));
  EXPECT_EQ(*++It, N3);
  EXPECT_NE(It, Copy);
  EXPECT_EQ(*++It, N2);
  EXPECT_EQ(It, Copy);
  EXPECT_EQ(++Copy, E);
  EXPECT_NE(It, E);
  EXPECT_EQ(++It, E);
}

TEST_F(DepthFirstIteratorGraphTest, PostIncrement) {
  // Post-increment returns a copy of the previous state.
  auto B = df_begin(G);
  auto It = B;
  auto Old = It++;
  EXPECT_EQ(Old, B);
  EXPECT_EQ(*Old, N0);
  EXPECT_EQ(*It, N1);
  EXPECT_EQ(*++Old, N1);
  EXPECT_EQ(Old, It);
}

TEST_F(DepthFirstIteratorGraphTest, PathAndNodeVisited) {
  auto It = df_begin(G), E = df_end(G);
  EXPECT_EQ(It.getPathLength(), 1u);
  EXPECT_EQ(It.getPath(0), N0);
  EXPECT_TRUE(It.nodeVisited(N0));
  EXPECT_FALSE(It.nodeVisited(N1));
  EXPECT_EQ(E.getPathLength(), 0u);
  EXPECT_FALSE(E.nodeVisited(N0));

  ++It;
  ++It;
  EXPECT_EQ(*It, N3);
  EXPECT_EQ(It.getPathLength(), 3u);
  EXPECT_EQ(It.getPath(0), N0);
  EXPECT_EQ(It.getPath(1), N1);
  EXPECT_EQ(It.getPath(2), N3);
  EXPECT_TRUE(It.nodeVisited(N3));
  EXPECT_FALSE(It.nodeVisited(N2));

  ++It;
  EXPECT_EQ(*It, N2);
  EXPECT_EQ(It.getPathLength(), 2u);
  EXPECT_EQ(It.getPath(1), N2);

  // The visited set is kept at the end, also in copies.
  ++It;
  EXPECT_EQ(It, E);
  EXPECT_EQ(It.getPathLength(), 0u);
  auto Copy = It;
  for (NodeT *N : {N0, N1, N2, N3}) {
    EXPECT_TRUE(It.nodeVisited(N));
    EXPECT_TRUE(Copy.nodeVisited(N));
  }
  EXPECT_FALSE(It.nodeVisited(N4));
  EXPECT_FALSE(Copy.nodeVisited(N4));
}

TEST_F(DepthFirstIteratorGraphTest, SkipChildren) {
  // Skipping the children of the root ends the traversal.
  auto It = df_begin(G), E = df_end(G);
  It.skipChildren();
  EXPECT_EQ(It, E);
  EXPECT_TRUE(It.nodeVisited(N0));
  EXPECT_FALSE(It.nodeVisited(N1));

  It = df_begin(G);
  EXPECT_EQ(*++It, N1);
  It.skipChildren();
  EXPECT_EQ(*It, N2);
  EXPECT_FALSE(It.nodeVisited(N3));
  It.skipChildren();
  EXPECT_EQ(It, E);
}

TEST_F(DepthFirstIteratorGraphTest, MarkVisited) {
  // Marking a node visited at the root keeps the iterator at the root.
  auto It = df_begin(G), E = df_end(G);
  It.markVisited(N1);
  EXPECT_EQ(It, df_begin(G));
  EXPECT_EQ(*It, N0);
  EXPECT_EQ(It.getPathLength(), 1u);
  EXPECT_TRUE(It.nodeVisited(N1));
  auto Copy = It;
  EXPECT_EQ(*++It, N2);
  EXPECT_EQ(++It, E);
  EXPECT_FALSE(It.nodeVisited(N3));
  EXPECT_EQ(*++Copy, N2);

  // Marking a node visited during the traversal.
  It = df_begin(G);
  EXPECT_EQ(*++It, N1);
  It.markVisited(N2);
  EXPECT_EQ(*++It, N3);
  EXPECT_EQ(++It, E);
  EXPECT_TRUE(It.nodeVisited(N2));
}

TEST_F(DepthFirstIteratorGraphTest, ExternalStorage) {
  // The begin iterator adds the root to the set.
  df_iterator_default_set<NodeT *> Visited;
  auto B = df_ext_begin(G, Visited), E = df_ext_end(G, Visited);
  EXPECT_TRUE(Visited.contains(N0));
  EXPECT_EQ(Visited.size(), 1u);
  EXPECT_NE(B, E);
  EXPECT_EQ(*B, N0);
  EXPECT_EQ(B.getPathLength(), 1u);
  EXPECT_TRUE(B.nodeVisited(N0));

  // External storage is single-pass: collect the nodes in one pass.
  SmallVector<NodeT *> Nodes;
  for (auto It = B; It != E; ++It)
    Nodes.push_back(*It);
  EXPECT_EQ(Nodes, (SmallVector<NodeT *>{N0, N1, N3, N2}));
  EXPECT_FALSE(Visited.contains(N4));

  // A traversal from a visited root is empty.
  EXPECT_EQ(df_ext_begin(G, Visited), df_ext_end(G, Visited));
  EXPECT_TRUE(depth_first_ext(G, Visited).empty());

  // Nodes in the set are skipped.
  Visited.clear();
  Visited.insert(N1);
  Nodes.clear();
  for (NodeT *N : depth_first_ext(G, Visited))
    Nodes.push_back(N);
  EXPECT_EQ(Nodes, (SmallVector<NodeT *>{N0, N2}));
}

TEST(DepthFirstIteratorTest, EndComparison) {
  Graph<1> G;
  auto B = df_begin(G), E = df_end(G);
  EXPECT_NE(B, E);
  EXPECT_EQ(E, df_end(G));
  EXPECT_FALSE(depth_first(G).empty());
  auto It = B;
  EXPECT_EQ(++It, E);
  EXPECT_NE(It, B);
  EXPECT_EQ(std::distance(B, E), 1);
}

TEST(DepthFirstIteratorTest, SameRootComparison) {
  // Two traversals from the same root are equal until they diverge.
  Graph<3> G;
  G.AddEdge(0, 1);
  G.AddEdge(0, 2);
  auto B1 = df_begin(G), B2 = df_begin(G);
  EXPECT_EQ(B1, B2);
  ++B1;
  EXPECT_NE(B1, B2);
  ++B2;
  EXPECT_EQ(B1, B2);
  EXPECT_EQ(std::distance(df_begin(G), df_end(G)), 3);
}
} // namespace llvm
