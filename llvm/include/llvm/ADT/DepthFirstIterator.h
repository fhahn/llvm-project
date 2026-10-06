//===- llvm/ADT/DepthFirstIterator.h - Depth First iterator -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file builds on the ADT/GraphTraits.h file to build generic depth
/// first graph iterator.  This file exposes the following functions/types:
///
/// df_begin/df_end/df_iterator
///   * Normal depth-first iteration - visit a node and then all of its
///     children.
///
/// idf_begin/idf_end/idf_iterator
///   * Depth-first iteration on the 'inverse' graph.
///
/// df_ext_begin/df_ext_end/df_ext_iterator
///   * Normal depth-first iteration - visit a node and then all of its
///     children. This iterator stores the 'visited' set in an external set,
///     which allows it to be more efficient, and allows external clients to
///     use the set for other purposes.
///
/// idf_ext_begin/idf_ext_end/idf_ext_iterator
///   * Depth-first iteration on the 'inverse' graph.
///     This iterator stores the 'visited' set in an external set, which
///     allows it to be more efficient, and allows external clients to use
///     the set for other purposes.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_DEPTHFIRSTITERATOR_H
#define LLVM_ADT_DEPTHFIRSTITERATOR_H

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"
#include <cassert>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace llvm {

// df_iterator_storage - A private class which is used to figure out where to
// store the visited set. A non-external set is part of the traversal state of
// df_iterator instead.
template<class SetType, bool External>   // Non-external set
class df_iterator_storage {};

template<class SetType>
class df_iterator_storage<SetType, true> {
public:
  df_iterator_storage(SetType &VSet) : Visited(VSet) {}
  df_iterator_storage(const df_iterator_storage &S) : Visited(S.Visited) {}

  SetType &Visited;
};

// The visited stated for the iteration is a simple set augmented with
// one more method, completed, which is invoked when all children of a
// node have been processed. It is intended to distinguish of back and
// cross edges in the spanning tree but is not used in the common case.
template <typename NodeRef, unsigned SmallSize = 8>
struct df_iterator_default_set : SmallPtrSet<NodeRef, SmallSize> {
  using BaseSet = SmallPtrSet<NodeRef, SmallSize>;
  using iterator = typename BaseSet::iterator;

  std::pair<iterator,bool> insert(NodeRef N) { return BaseSet::insert(N); }
  template <typename IterT>
  void insert(IterT Begin, IterT End) { BaseSet::insert(Begin,End); }

  void completed(NodeRef) {}
};

// Generic Depth First Iterator
template <class GraphT,
          class SetType =
              df_iterator_default_set<typename GraphTraits<GraphT>::NodeRef>,
          bool ExtStorage = false, class GT = GraphTraits<GraphT>>
class df_iterator : public df_iterator_storage<SetType, ExtStorage> {
public:
  // When External storage is used we are not multi-pass safe.
  using iterator_category =
      std::conditional_t<ExtStorage, std::input_iterator_tag,
                         std::forward_iterator_tag>;
  using value_type = typename GT::NodeRef;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type *;
  using reference = const value_type &;

private:
  using NodeRef = typename GT::NodeRef;
  using ChildItTy = typename GT::ChildIteratorType;

  // First element is node reference, second is the 'next child' to visit.
  // The second child is initialized lazily to pick up graph changes during the
  // DFS.
  using StackElement = std::pair<NodeRef, std::optional<ChildItTy>>;

  // The state of a traversal that has started at Root. With an external set,
  // the set already contains Root.
  struct StackState {
    // VisitStack - Used to maintain the ordering.  Top = current block
    SmallVector<StackElement, 8> VisitStack;

    explicit StackState(NodeRef Root) {
      VisitStack.push_back(StackElement(Root, std::nullopt));
    }
  };
  struct StackAndSetState : StackState {
    SetType Visited;

    explicit StackAndSetState(NodeRef Root) : StackState(Root) {
      Visited.insert(Root);
    }
  };

  // The traversal state is created when the iterator is first incremented or
  // modified. Until then, the iterator only holds the root in PendingRoot, so
  // that copying it is cheap: iterator adaptors copy iterators many times
  // before using them. At most one of PendingRoot and State is engaged.
  std::optional<NodeRef> PendingRoot;
  std::optional<std::conditional_t<ExtStorage, StackState, StackAndSetState>>
      State;

  inline df_iterator(NodeRef Node) : PendingRoot(Node) {}

  // End is when there is no pending root and the stack is empty or absent.
  inline df_iterator() = default;

  inline df_iterator(NodeRef Node, SetType &S)
      : df_iterator_storage<SetType, ExtStorage>(S) {
    if (this->Visited.insert(Node).second)
      PendingRoot = Node;
  }

  inline df_iterator(SetType &S)
    : df_iterator_storage<SetType, ExtStorage>(S) {
    // End is when there is no pending root and the stack is empty or absent.
  }

  SetType &visited() {
    if constexpr (ExtStorage)
      return this->Visited;
    else
      return State->Visited;
  }

  void startTraversal() {
    assert(PendingRoot && !State && "Traversal already started or at the end");
    State.emplace(*PendingRoot);
    PendingRoot.reset();
  }

  inline void toNext() {
    SmallVectorImpl<StackElement> &VisitStack = State->VisitStack;
    do {
      NodeRef Node = VisitStack.back().first;
      std::optional<ChildItTy> &Opt = VisitStack.back().second;

      if (!Opt)
        Opt.emplace(GT::child_begin(Node));

      // Notice that we directly mutate *Opt here, so that
      // VisitStack.back().second actually gets updated as the iterator
      // increases.
      while (*Opt != GT::child_end(Node)) {
        NodeRef Next = *(*Opt)++;
        // Has our next sibling been visited?
        if (visited().insert(Next).second) {
          // No, do it now.
          VisitStack.push_back(StackElement(Next, std::nullopt));
          return;
        }
      }
      visited().completed(Node);

      // Oops, ran out of successors... go up a level on the stack.
      VisitStack.pop_back();
    } while (!VisitStack.empty());
  }

public:
  // Provide static begin and end methods as our public "constructors"
  static df_iterator begin(const GraphT &G) {
    return df_iterator(GT::getEntryNode(G));
  }
  static df_iterator end(const GraphT &G) { return df_iterator(); }

  // Static begin and end methods as our public ctors for external iterators
  static df_iterator begin(const GraphT &G, SetType &S) {
    return df_iterator(GT::getEntryNode(G), S);
  }
  static df_iterator end(const GraphT &G, SetType &S) { return df_iterator(S); }

  bool operator==(const df_iterator &x) const {
    if (State && x.State)
      return State->VisitStack == x.State->VisitStack;
    if (!State && !x.State)
      return PendingRoot == x.PendingRoot;
    // An iterator that has not started stands for the stack
    // [(PendingRoot, nullopt)]. A started iterator only has such a stack if
    // markVisited started it before the first increment.
    const auto &Stack = State ? State->VisitStack : x.State->VisitStack;
    const std::optional<NodeRef> &R = State ? x.PendingRoot : PendingRoot;
    return R ? Stack.size() == 1 && Stack[0].first == *R && !Stack[0].second
             : Stack.empty();
  }
  bool operator!=(const df_iterator &x) const { return !(*this == x); }

  reference operator*() const {
    return State ? State->VisitStack.back().first : *PendingRoot;
  }

  // This is a nonstandard operator-> that dereferences the pointer an extra
  // time... so that you can actually call methods ON the Node, because
  // the contained type is a pointer.  This allows BBIt->getTerminator() f.e.
  //
  NodeRef operator->() const { return **this; }

  df_iterator &operator++() { // Preincrement
    if (!State)
      startTraversal();
    toNext();
    return *this;
  }

  /// Skips all children of the current node and traverses to next node
  ///
  /// Note: This function takes care of incrementing the iterator. If you
  /// always increment and call this function, you risk walking off the end.
  df_iterator &skipChildren() {
    if (!State)
      startTraversal();
    State->VisitStack.pop_back();
    if (!State->VisitStack.empty())
      toNext();
    return *this;
  }

  df_iterator operator++(int) { // Postincrement
    df_iterator tmp = *this;
    ++*this;
    return tmp;
  }

  /// Mark \p Node as visited, so that the traversal does not visit it and the
  /// nodes that are only reachable through it. Must not be called on an end
  /// iterator with an internal visited set.
  void markVisited(NodeRef Node) {
    if constexpr (!ExtStorage)
      if (!State)
        startTraversal();
    visited().insert(Node);
  }

  // nodeVisited - return true if this iterator has already visited the
  // specified node.  This is public, and will probably be used to iterate over
  // nodes that a depth first iteration did not find: ie unreachable nodes.
  //
  bool nodeVisited(NodeRef Node) const {
    if constexpr (ExtStorage)
      return this->Visited.contains(Node);
    else
      return State ? State->Visited.contains(Node) : PendingRoot == Node;
  }

  /// Return the length of the path from the entry node to the current node,
  /// counting both nodes.
  unsigned getPathLength() const {
    if (State)
      return State->VisitStack.size();
    return PendingRoot ? 1 : 0;
  }

  /// Return the n'th node in the path from the entry node to the current node.
  NodeRef getPath(unsigned n) const {
    assert(n < getPathLength() && "Path index out of range");
    return State ? State->VisitStack[n].first : *PendingRoot;
  }
};

// Provide global constructors that automatically figure out correct types...
//
template <class T>
df_iterator<T> df_begin(const T& G) {
  return df_iterator<T>::begin(G);
}

template <class T>
df_iterator<T> df_end(const T& G) {
  return df_iterator<T>::end(G);
}

// Provide an accessor method to use them in range-based patterns.
template <class T>
iterator_range<df_iterator<T>> depth_first(const T& G) {
  return make_range(df_begin(G), df_end(G));
}

// Provide global definitions of external depth first iterators...
template <class T,
          class SetTy =
              df_iterator_default_set<typename GraphTraits<T>::NodeRef>>
struct df_ext_iterator : df_iterator<T, SetTy, true> {
  df_ext_iterator(const df_iterator<T, SetTy, true> &V)
    : df_iterator<T, SetTy, true>(V) {}
};

template <class T, class SetTy>
df_ext_iterator<T, SetTy> df_ext_begin(const T& G, SetTy &S) {
  return df_ext_iterator<T, SetTy>::begin(G, S);
}

template <class T, class SetTy>
df_ext_iterator<T, SetTy> df_ext_end(const T& G, SetTy &S) {
  return df_ext_iterator<T, SetTy>::end(G, S);
}

template <class T, class SetTy>
iterator_range<df_ext_iterator<T, SetTy>> depth_first_ext(const T& G,
                                                          SetTy &S) {
  return make_range(df_ext_begin(G, S), df_ext_end(G, S));
}

// Provide global definitions of inverse depth first iterators...
template <class T,
          class SetTy =
              df_iterator_default_set<typename GraphTraits<T>::NodeRef>,
          bool External = false>
struct idf_iterator : df_iterator<Inverse<T>, SetTy, External> {
  idf_iterator(const df_iterator<Inverse<T>, SetTy, External> &V)
    : df_iterator<Inverse<T>, SetTy, External>(V) {}
};

template <class T>
idf_iterator<T> idf_begin(const T& G) {
  return idf_iterator<T>::begin(Inverse<T>(G));
}

template <class T>
idf_iterator<T> idf_end(const T& G){
  return idf_iterator<T>::end(Inverse<T>(G));
}

// Provide an accessor method to use them in range-based patterns.
template <class T>
iterator_range<idf_iterator<T>> inverse_depth_first(const T& G) {
  return make_range(idf_begin(G), idf_end(G));
}

// Provide global definitions of external inverse depth first iterators...
template <class T,
          class SetTy =
              df_iterator_default_set<typename GraphTraits<T>::NodeRef>>
struct idf_ext_iterator : idf_iterator<T, SetTy, true> {
  idf_ext_iterator(const idf_iterator<T, SetTy, true> &V)
    : idf_iterator<T, SetTy, true>(V) {}
  idf_ext_iterator(const df_iterator<Inverse<T>, SetTy, true> &V)
    : idf_iterator<T, SetTy, true>(V) {}
};

template <class T, class SetTy>
idf_ext_iterator<T, SetTy> idf_ext_begin(const T& G, SetTy &S) {
  return idf_ext_iterator<T, SetTy>::begin(Inverse<T>(G), S);
}

template <class T, class SetTy>
idf_ext_iterator<T, SetTy> idf_ext_end(const T& G, SetTy &S) {
  return idf_ext_iterator<T, SetTy>::end(Inverse<T>(G), S);
}

template <class T, class SetTy>
iterator_range<idf_ext_iterator<T, SetTy>> inverse_depth_first_ext(const T& G,
                                                                   SetTy &S) {
  return make_range(idf_ext_begin(G, S), idf_ext_end(G, S));
}

} // end namespace llvm

#endif // LLVM_ADT_DEPTHFIRSTITERATOR_H
