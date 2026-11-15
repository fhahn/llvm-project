//===-- CostTable.h - Instruction Cost Table handling -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cost tables and simple lookup functions
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_COSTTABLE_H_
#define LLVM_CODEGEN_COSTTABLE_H_

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/bit.h"
#include "llvm/CodeGenTypes/MachineValueType.h"
#include <array>
#include <cstdint>
#include <type_traits>

namespace llvm {

/// Cost Table Entry
template <typename CostType>
struct CostTblEntryT {
  uint16_t ISD;
  MVT::SimpleValueType Type;
  CostType Cost;
};
using CostTblEntry = CostTblEntryT<uint16_t>;

/// Find in cost table.
template <class CostType>
inline const CostTblEntryT<CostType> *
CostTableLookup(ArrayRef<CostTblEntryT<CostType>> Tbl, int ISD, MVT Ty) {
  auto I = find_if(Tbl, [=](const CostTblEntryT<CostType> &Entry) {
    return ISD == Entry.ISD && Ty == Entry.Type;
  });
  if (I != Tbl.end())
    return I;

  // Could not find an entry.
  return nullptr;
}

template <size_t N, class CostType>
inline const CostTblEntryT<CostType> *
CostTableLookup(const CostTblEntryT<CostType> (&Table)[N], int ISD, MVT Ty) {
  // Wrapper to fix template argument deduction failures.
  return CostTableLookup<CostType>(Table, ISD, Ty);
}

/// Type Conversion Cost Table
template <typename CostType>
struct TypeConversionCostTblEntryT {
  uint16_t ISD;
  MVT::SimpleValueType Dst;
  MVT::SimpleValueType Src;
  CostType Cost;
};
using TypeConversionCostTblEntry = TypeConversionCostTblEntryT<uint16_t>;

/// Find in type conversion cost table.
template <class CostType>
inline const TypeConversionCostTblEntryT<CostType> *
ConvertCostTableLookup(ArrayRef<TypeConversionCostTblEntryT<CostType>> Tbl,
                       int ISD, MVT Dst, MVT Src) {
  auto I =
      find_if(Tbl, [=](const TypeConversionCostTblEntryT<CostType> &Entry) {
        return ISD == Entry.ISD && Src == Entry.Src && Dst == Entry.Dst;
      });
  if (I != Tbl.end())
    return I;

  // Could not find an entry.
  return nullptr;
}

template <size_t N, class CostType>
inline const TypeConversionCostTblEntryT<CostType> *
ConvertCostTableLookup(const TypeConversionCostTblEntryT<CostType> (&Table)[N],
                       int ISD, MVT Dst, MVT Src) {
  // Wrapper to fix template argument deduction failures.
  return ConvertCostTableLookup<CostType>(Table, ISD, Dst, Src);
}

namespace detail {
/// Keys that are equal iff a cost table entry matches the lookup arguments.
inline uint64_t costTableKey(int ISD, MVT Ty) {
  return uint64_t(unsigned(ISD)) << 16 | Ty.SimpleTy;
}
inline uint64_t costTableKey(int ISD, MVT Dst, MVT Src) {
  return uint64_t(unsigned(ISD)) << 32 | uint64_t(Dst.SimpleTy) << 16 |
         Src.SimpleTy;
}
template <class CostType>
uint64_t costTableKey(const CostTblEntryT<CostType> &Entry) {
  return costTableKey(Entry.ISD, Entry.Type);
}
template <class CostType>
uint64_t costTableKey(const TypeConversionCostTblEntryT<CostType> &Entry) {
  return costTableKey(Entry.ISD, Entry.Dst, Entry.Src);
}

/// Hash index from the keys of a cost table of type \p TableT, an array of
/// entries, to the first entry with that key, the entry a linear scan finds.
template <class TableT> class CostTableIndex {
  using EntryT = std::remove_extent_t<TableT>;
  static constexpr size_t N = std::extent_v<TableT>;
  static_assert(N < UINT16_MAX, "Cost table too large to index");
  // Linear probing in a table that is at most a quarter full.
  static constexpr unsigned Bits = bit_width_constexpr(4 * N - 1);
  // One plus the index of an entry, zero for an empty slot.
  std::array<uint16_t, size_t(1) << Bits> Slots = {};

  static size_t slot(uint64_t Key) {
    return (Key * 0x9E3779B97F4A7C15ULL) >> (64 - Bits);
  }
  static size_t next(size_t Slot) { return (Slot + 1) % (size_t(1) << Bits); }

public:
  explicit CostTableIndex(const TableT &Tbl) {
    for (size_t I = 0; I != N; ++I) {
      uint64_t Key = costTableKey(Tbl[I]);
      size_t S = slot(Key);
      while (Slots[S] && costTableKey(Tbl[Slots[S] - 1]) != Key)
        S = next(S);
      if (!Slots[S])
        Slots[S] = I + 1;
    }
  }

  EntryT *lookup(const TableT &Tbl, uint64_t Key) const {
    for (size_t S = slot(Key); Slots[S]; S = next(S))
      if (costTableKey(Tbl[Slots[S] - 1]) == Key)
        return &Tbl[Slots[S] - 1];
    return nullptr;
  }
};

template <auto &Tbl> auto *indexedCostTableLookup(uint64_t Key) {
  using TableT = std::remove_reference_t<decltype(Tbl)>;
  static_assert(std::is_const_v<TableT>, "Only constant tables can be indexed");
  static const CostTableIndex<TableT> Index(Tbl);
  return Index.lookup(Tbl, Key);
}
} // namespace detail

/// Find in cost table \p Tbl, a constant array with static storage duration,
/// through an index that the first lookup builds.
template <auto &Tbl> auto *CostTableLookup(int ISD, MVT Ty) {
  return detail::indexedCostTableLookup<Tbl>(detail::costTableKey(ISD, Ty));
}

/// Find in type conversion cost table \p Tbl, a constant array with static
/// storage duration, through an index that the first lookup builds.
template <auto &Tbl> auto *ConvertCostTableLookup(int ISD, MVT Dst, MVT Src) {
  return detail::indexedCostTableLookup<Tbl>(
      detail::costTableKey(ISD, Dst, Src));
}

} // namespace llvm

#endif /* LLVM_CODEGEN_COSTTABLE_H_ */
