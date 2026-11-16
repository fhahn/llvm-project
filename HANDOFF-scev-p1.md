# Handoff: SCEV loop-guard flag leak fix via SCEVUse use flags

DO NOT UPSTREAM this file; drop this commit before submitting.

## Goal

Fix the miscompile where `SCEVLoopGuardRewriter` (`LoopGuards::rewrite`) puts nuw/nsw that only hold under the loop
guards onto uniqued SCEV nodes (4812e9a48773), which are shared with unguarded values. Submit upstream as a clean
series. Owner's bar: simple, clean, correct, no precision loss vs origin/main, every piece justified by a test, use
flags flow through the normal APIs (no separate entry point like `getRangeOfUse`, no per-site `.getPointer()`
workarounds), no compile-time regression.

Example (also in the fix commit message): in `@src`, the guard `%a == -1` of an inner loop maps `%a` to -1; computing
its constant max BTC rewrites `(%a + %p)<nuw>` to `(-1 + %p)<nuw>`, which is also the SCEV of the unguarded
`%r = add i8 %p, -1`; IndVars then adds nuw to `%r` (poison for 32640 of 32896 defined i8 inputs, llubi).

## Design (decided with the owner)

1. Guard-only flags become **use flags** on the `SCEVUse` the rewriter returns (`SCEVFlagsPair{FlagNone, flags}`).
   Rebuilt 2-operand adds/muls get all of the original's flags; 3+ operands get nuw only (n-ary nsw depends on operand
   order, which `getAddExpr` changes by sorting/folding: that was a pre-existing miscompile, now fixed and tested).
2. **One range engine**: `getRangeRef` and the public range helpers take `SCEVUse` and read `S.getNoWrapFlags()`;
   operand uses are part of node identity, so per-node caching is sound; ranges are keyed by use.
3. **Restrict at copy** (chosen over "restrict at creation", which loses `<nw>` on shipped tests): a node's flags may
   depend on its operands' use flags; they are copied onto a rebuild only if every old operand is canonical or kept as
   the identical use (`getNoWrapFlagsForRebuild(NewOps)`); sites: `getWithOperands` (covers `computeAndSetCanonical`
   and at-scope), `SCEVRewriteVisitor::visitAddRecExpr`, `SCEVCastSinkingRewriter`, at-scope AddRec nw, the folds into
   AddRecs in `getAddExpr`/`getMulExpr`. Extend rebuilds keep start/step uses; the pre-start bails on use-flagged
   start/step.
4. `applyLoopGuards` returns `SCEVUse`; consumers that reach range/predicate queries keep it.

## Branches (all on the owner's origin/main 01403ba523a5 unless noted)

- `scev-prec/p1-copy-v3` = `scev-prec/p1-copy-v3-review3` (tip **0b064684bf74**): COMPLETE, fully reviewed
  6-commit stack (tests NFC, visitor NFC, copy rule, ranges, fix, consumers). Lit = base (only 4 DirectX fails);
  CTMark -O3 release compile time vs origin/main: **-0.046%** total, no program slower. Final review-llvm (all 5
  lanes): no soundness issue; ~1,530 generated kernels under llubi, 0 mismatches (base: miscompiles); leak scan
  base 77/252, tip 0. Use this if the rework below is abandoned.
- `scev-prec/p1-copy-v4-wip` (this branch): the last fix round, INTERRUPTED. Commits:
  1. `ad32b09c935d` tests (NFC) — adds `rewrite_preserve_add_nsw_constant_max_other_guard` (item 5 test; base
     constant max 2147483647, fixed tip 2147483646). Verified: regenerating all four files' checks at this commit
     with an origin/main opt gives no diff.
  2. `7b9f0212dc03` visitor: `visit` returns SCEVUse, new `visitOperand` keeps the operand use when unchanged, one
     comparison everywhere (items 2+3). Not NFC-labelled any more (message says why). Built, lit 0 fails, CTMark
     print<scalar-evolution> identical to base.
  3. `ed2e5871f98f` copy rule (operand-aware), at-scope keeps unchanged operand uses (item 3), wording uses
     "not canonical" (item 4). Built, lit 0 fails, corpus identical to previous commit.
  4. `bf5f9b4f75b5` ranges; range caches keyed by `SCEVUse`, `eraseRanges` erases flagged variants (item 7). Built,
     lit 0 fails; corpus: 5 files with tighter at-scope exit-value ranges (gains).
  5. `043596b3064a` WIP consumers (item 1: all consumer plumbing before the fix). NOT built or tested.
  6. this handoff commit.
- Older: `scev-prec/p1-copy-v2`, `scev-prec/p1-copy-v3-presimplify` (pre-simplification), `scev-prec/p1-struct-copy`,
  `scev-prec/p1-struct-creation` (rejected alternative), `scev-prec/p1-clean` (overload + strips version, superseded),
  `scev-prec/p1-useflags` (original, getRangeOfUse).

## Remaining work on p1-copy-v4-wip

1. Drop this commit. Build + test commit 5 (consumers): should be NFC for guards (rewriter returns no use flags
   yet); if lit/corpus show no change, label it NFC; motivate by exit-value uses reaching isKnown*/ranges.
2. Redo the fix commit on top: cherry-pick `318da1b8e5f8` (the fix from v3) — it conflicts in ScalarEvolution.h
   because the return-type changes moved to commit 5; resolve by keeping only the rewriter changes + PreserveNUW/NSW
   removal. Then:
   - rename the rewriter helper `getUseFlags` -> e.g. `getFlagsUnderGuards` (clashes with `SCEVUse::getUseFlags`);
   - assert in `getAddExpr`/`getMulExpr` that n-ary nsw is never passed as a use flag
     (`Ops.size() <= 2 || !hasFlags(UseFlags, FlagNSW)`, like `evaluateAtIteration`), check lit;
   - fix message: one or two sentences on the known limitation (owner decided not to convert): consumers that still
     store `applyLoopGuards` results as `const SCEV *` (LoopPeel BTC, LV urem, trip multiple, RewriteMap values,
     LoopIdiom, LAA MaxBTC/DerefBytes) and folds reading node flags (zext/sext distribution, udiv, constant multiple)
     no longer see guard flags; mention MaxOffset next to StartOffset in the consumer message.
   - expected test deltas in the fix commit only: `rewrite_flags_only_hold_under_guard{,_mul}` (nuw gone in unguarded
     loop), IndVars `rewrite_flags_only_hold_under_guard{,_exit_value,_strengthen}` (miscompiles fixed),
     `rewrite_nary_add_nsw_folds_constants` (constant max exact), `..._other_guard` (2147483646).
     `rewrite_preserve_add_nsw_step` must NOT flip (that was the point of moving the consumers first).
3. Remaining wording nits from the final review: getRangeRefIter comment ("Operand uses with flags are not queued;
   their ranges are computed when the ranges of their users are"), hoist `S.getNoWrapFlags()` in getRangeRef's AddRec
   case, clearer sentence instead of "Ranges of canonical expressions do not change." in the ranges message.
4. Verify per commit (build, lit Analysis/Transforms/CodeGen, check lines change only where behaviour changes; test
   commit checks regenerate identically with a base opt), CTMark print<scalar-evolution> pre-IndVars/pre-LV vs v3
   tip 0b064684bf74, llubi differential + leak generators, then a release CTMark compile-time A/B vs origin/main
   (range cache rework changed the hot path; v3 was -0.046%).
5. Optional: one more review-llvm pass on the final stack.

## Review history (what was rejected and why)

- getRangeOfUse as a separate entry point: owner rejected ("getRange should do the right thing for SCEVUse").
- `const SCEV *` / `SCEVUse` overloads + 9 `.getPointer()` strips at inference sites: fragile (any future
  const SCEV* -> SCEVUse conversion reopens the leak).
- Restrict at creation (non-canonical nodes only get their canonical's flags): loses `<nw>` on recurrences starting
  at exit values (shipped test exit-value-nowrap-flags.ll), 11 CTMark rows worse.
- Dropping all transferred flags: regresses rewrite_preserve_add_nsw* and real code.
