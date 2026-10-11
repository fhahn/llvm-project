; RUN: not llubi --verbose --entry-function=load_oob_after < %s 2>&1 | FileCheck %s --check-prefix=LOAD-OOB-AFTER
; RUN: not llubi --verbose --entry-function=load_oob_before < %s 2>&1 | FileCheck %s --check-prefix=LOAD-OOB-BEFORE
; RUN: not llubi --verbose --entry-function=load_misaligned < %s 2>&1 | FileCheck %s --check-prefix=LOAD-MISALIGNED
; RUN: not llubi --verbose --entry-function=load_poison_ptr < %s 2>&1 | FileCheck %s --check-prefix=LOAD-POISON-PTR
; RUN: not llubi --verbose --entry-function=load_null < %s 2>&1 | FileCheck %s --check-prefix=LOAD-NULL
; RUN: not llubi --verbose --entry-function=load_freed < %s 2>&1 | FileCheck %s --check-prefix=LOAD-FREED
; RUN: not llubi --verbose --entry-function=load_poison_mask_lane_oob < %s 2>&1 | FileCheck %s --check-prefix=LOAD-POISON-MASK-LANE-OOB
; RUN: not llubi --verbose --entry-function=load_poison_mask_lane_poison_ptr < %s 2>&1 | FileCheck %s --check-prefix=LOAD-POISON-MASK-LANE-POISON-PTR
; RUN: not llubi --verbose --entry-function=store_oob_after < %s 2>&1 | FileCheck %s --check-prefix=STORE-OOB-AFTER
; RUN: not llubi --verbose --entry-function=store_dead < %s 2>&1 | FileCheck %s --check-prefix=STORE-DEAD
; RUN: not llubi --verbose --entry-function=store_constant_mem < %s 2>&1 | FileCheck %s --check-prefix=STORE-CONSTANT-MEM
; RUN: not llubi --verbose --entry-function=store_all_poison_mask_oob < %s 2>&1 | FileCheck %s --check-prefix=STORE-ALL-POISON-MASK-OOB
; RUN: not llubi --verbose --entry-function=store_poison_mask_lane_constant_mem < %s 2>&1 | FileCheck %s --check-prefix=STORE-POISON-MASK-LANE-CONSTANT-MEM

@a = global [4 x i32] [i32 0, i32 1, i32 2, i32 3]
@c = constant [4 x i32] [i32 0, i32 1, i32 2, i32 3]

define void @load_oob_after() {
; LOAD-OOB-AFTER: Entering function: load_oob_after
; LOAD-OOB-AFTER-NEXT:   %p = getelementptr i32, ptr @a, i64 2 => ptr 0x10 [@a + 8]
; LOAD-OOB-AFTER-NEXT: Stacktrace:
; LOAD-OOB-AFTER-NEXT: #0   %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 true, i1 false, i1 true, i1 false>, <4 x i32> poison) at @load_oob_after <stdin>:{{[0-9]+}}
; LOAD-OOB-AFTER-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 12, Address: 0x10, Object base: 0x8, Object size: 16.
; LOAD-OOB-AFTER-NEXT: error: Execution of function 'load_oob_after' failed.
  %p = getelementptr i32, ptr @a, i64 2
  %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 true, i1 false, i1 true, i1 false>, <4 x i32> poison)
  ret void
}

define void @load_oob_before() {
; LOAD-OOB-BEFORE: Entering function: load_oob_before
; LOAD-OOB-BEFORE-NEXT:   %p = getelementptr i32, ptr @a, i64 -1 => ptr 0x4 [@a + -4]
; LOAD-OOB-BEFORE-NEXT: Stacktrace:
; LOAD-OOB-BEFORE-NEXT: #0   %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 true, i1 true, i1 false, i1 false>, <4 x i32> poison) at @load_oob_before <stdin>:{{[0-9]+}}
; LOAD-OOB-BEFORE-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 8, Address: 0x4, Object base: 0x8, Object size: 16.
; LOAD-OOB-BEFORE-NEXT: error: Execution of function 'load_oob_before' failed.
  %p = getelementptr i32, ptr @a, i64 -1
  %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 true, i1 true, i1 false, i1 false>, <4 x i32> poison)
  ret void
}

; The align attribute makes the misaligned pointer poison.
define void @load_misaligned() {
; LOAD-MISALIGNED: Entering function: load_misaligned
; LOAD-MISALIGNED-NEXT:   %p = getelementptr i32, ptr @a, i64 1 => ptr 0xC [@a + 4]
; LOAD-MISALIGNED-NEXT: Stacktrace:
; LOAD-MISALIGNED-NEXT: #0   %r = call <2 x i32> @llvm.masked.load.v2i32.p0(ptr align 8 %p, <2 x i1> <i1 false, i1 true>, <2 x i32> poison) at @load_misaligned <stdin>:{{[0-9]+}}
; LOAD-MISALIGNED-NEXT: Immediate UB detected: llvm.masked.load with poison pointer.
; LOAD-MISALIGNED-NEXT: error: Execution of function 'load_misaligned' failed.
  %p = getelementptr i32, ptr @a, i64 1
  %r = call <2 x i32> @llvm.masked.load.v2i32.p0(ptr align 8 %p, <2 x i1> <i1 false, i1 true>, <2 x i32> poison)
  ret void
}

define void @load_poison_ptr() {
; LOAD-POISON-PTR: Entering function: load_poison_ptr
; LOAD-POISON-PTR-NEXT: Stacktrace:
; LOAD-POISON-PTR-NEXT: #0   %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 poison, <4 x i1> <i1 false, i1 false, i1 false, i1 true>, <4 x i32> poison) at @load_poison_ptr <stdin>:{{[0-9]+}}
; LOAD-POISON-PTR-NEXT: Immediate UB detected: llvm.masked.load with poison pointer.
; LOAD-POISON-PTR-NEXT: error: Execution of function 'load_poison_ptr' failed.
  %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 poison, <4 x i1> <i1 false, i1 false, i1 false, i1 true>, <4 x i32> poison)
  ret void
}

define void @load_null() {
; LOAD-NULL: Entering function: load_null
; LOAD-NULL-NEXT: Stacktrace:
; LOAD-NULL-NEXT: #0   %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 null, <4 x i1> <i1 false, i1 true, i1 false, i1 false>, <4 x i32> poison) at @load_null <stdin>:{{[0-9]+}}
; LOAD-NULL-NEXT: Immediate UB detected: Invalid memory access via a pointer with nullary provenance.
; LOAD-NULL-NEXT: error: Execution of function 'load_null' failed.
  %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 null, <4 x i1> <i1 false, i1 true, i1 false, i1 false>, <4 x i32> poison)
  ret void
}

define void @load_freed() {
; LOAD-FREED: Entering function: load_freed
; LOAD-FREED-NEXT:   %p = call ptr @malloc(i64 16) => ptr 0x30 [p]
; LOAD-FREED-NEXT:   call void @free(ptr %p)
; LOAD-FREED-NEXT: Stacktrace:
; LOAD-FREED-NEXT: #0   %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 false, i1 false, i1 true, i1 false>, <4 x i32> poison) at @load_freed <stdin>:{{[0-9]+}}
; LOAD-FREED-NEXT: Immediate UB detected: Try to access a dead memory object at address 0x38.
; LOAD-FREED-NEXT: error: Execution of function 'load_freed' failed.
  %p = call ptr @malloc(i64 16)
  call void @free(ptr %p)
  %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 false, i1 false, i1 true, i1 false>, <4 x i32> poison)
  ret void
}

; A poison mask lane may be enabled, so it must be in bounds.
define void @load_poison_mask_lane_oob() {
; LOAD-POISON-MASK-LANE-OOB: Entering function: load_poison_mask_lane_oob
; LOAD-POISON-MASK-LANE-OOB-NEXT:   %p = getelementptr i32, ptr @a, i64 2 => ptr 0x10 [@a + 8]
; LOAD-POISON-MASK-LANE-OOB-NEXT: Stacktrace:
; LOAD-POISON-MASK-LANE-OOB-NEXT: #0   %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 true, i1 false, i1 poison, i1 false>, <4 x i32> poison) at @load_poison_mask_lane_oob <stdin>:{{[0-9]+}}
; LOAD-POISON-MASK-LANE-OOB-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 12, Address: 0x10, Object base: 0x8, Object size: 16.
; LOAD-POISON-MASK-LANE-OOB-NEXT: error: Execution of function 'load_poison_mask_lane_oob' failed.
  %p = getelementptr i32, ptr @a, i64 2
  %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 %p, <4 x i1> <i1 true, i1 false, i1 poison, i1 false>, <4 x i32> poison)
  ret void
}

define void @load_poison_mask_lane_poison_ptr() {
; LOAD-POISON-MASK-LANE-POISON-PTR: Entering function: load_poison_mask_lane_poison_ptr
; LOAD-POISON-MASK-LANE-POISON-PTR-NEXT: Stacktrace:
; LOAD-POISON-MASK-LANE-POISON-PTR-NEXT: #0   %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 poison, <4 x i1> <i1 false, i1 poison, i1 false, i1 false>, <4 x i32> poison) at @load_poison_mask_lane_poison_ptr <stdin>:{{[0-9]+}}
; LOAD-POISON-MASK-LANE-POISON-PTR-NEXT: Immediate UB detected: llvm.masked.load with poison pointer.
; LOAD-POISON-MASK-LANE-POISON-PTR-NEXT: error: Execution of function 'load_poison_mask_lane_poison_ptr' failed.
  %r = call <4 x i32> @llvm.masked.load.v4i32.p0(ptr align 4 poison, <4 x i1> <i1 false, i1 poison, i1 false, i1 false>, <4 x i32> poison)
  ret void
}

define void @store_oob_after() {
; STORE-OOB-AFTER: Entering function: store_oob_after
; STORE-OOB-AFTER-NEXT:   %p = getelementptr i32, ptr @a, i64 2 => ptr 0x10 [@a + 8]
; STORE-OOB-AFTER-NEXT: Stacktrace:
; STORE-OOB-AFTER-NEXT: #0   call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 %p, <4 x i1> <i1 true, i1 false, i1 true, i1 false>) at @store_oob_after <stdin>:{{[0-9]+}}
; STORE-OOB-AFTER-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 12, Address: 0x10, Object base: 0x8, Object size: 16.
; STORE-OOB-AFTER-NEXT: error: Execution of function 'store_oob_after' failed.
  %p = getelementptr i32, ptr @a, i64 2
  call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 %p, <4 x i1> <i1 true, i1 false, i1 true, i1 false>)
  ret void
}

define void @store_dead() {
; STORE-DEAD: Entering function: store_dead
; STORE-DEAD-NEXT:   %p = alloca [4 x i32], align 4 => ptr 0x30 [p (dead)]
; STORE-DEAD-NEXT:   call void @llvm.lifetime.start.p0(ptr %p)
; STORE-DEAD-NEXT:   call void @llvm.lifetime.end.p0(ptr %p)
; STORE-DEAD-NEXT: Stacktrace:
; STORE-DEAD-NEXT: #0   call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 %p, <4 x i1> <i1 false, i1 true, i1 false, i1 false>) at @store_dead <stdin>:{{[0-9]+}}
; STORE-DEAD-NEXT: Immediate UB detected: Try to access a dead memory object at address 0x34.
; STORE-DEAD-NEXT: error: Execution of function 'store_dead' failed.
  %p = alloca [4 x i32], align 4
  call void @llvm.lifetime.start.p0(ptr %p)
  call void @llvm.lifetime.end.p0(ptr %p)
  call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 %p, <4 x i1> <i1 false, i1 true, i1 false, i1 false>)
  ret void
}

define void @store_constant_mem() {
; STORE-CONSTANT-MEM: Entering function: store_constant_mem
; STORE-CONSTANT-MEM-NEXT: Stacktrace:
; STORE-CONSTANT-MEM-NEXT: #0   call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 @c, <4 x i1> <i1 false, i1 true, i1 false, i1 false>) at @store_constant_mem <stdin>:{{[0-9]+}}
; STORE-CONSTANT-MEM-NEXT: Immediate UB detected: Try to write to a constant memory object: ptr 0x20 [@c + 4].
; STORE-CONSTANT-MEM-NEXT: error: Execution of function 'store_constant_mem' failed.
  call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 @c, <4 x i1> <i1 false, i1 true, i1 false, i1 false>)
  ret void
}

define void @store_all_poison_mask_oob() {
; STORE-ALL-POISON-MASK-OOB: Entering function: store_all_poison_mask_oob
; STORE-ALL-POISON-MASK-OOB-NEXT:   %p = getelementptr i32, ptr @a, i64 3 => ptr 0x14 [@a + 12]
; STORE-ALL-POISON-MASK-OOB-NEXT: Stacktrace:
; STORE-ALL-POISON-MASK-OOB-NEXT: #0   call void @llvm.masked.store.v2i32.p0(<2 x i32> zeroinitializer, ptr align 4 %p, <2 x i1> poison) at @store_all_poison_mask_oob <stdin>:{{[0-9]+}}
; STORE-ALL-POISON-MASK-OOB-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 8, Address: 0x14, Object base: 0x8, Object size: 16.
; STORE-ALL-POISON-MASK-OOB-NEXT: error: Execution of function 'store_all_poison_mask_oob' failed.
  %p = getelementptr i32, ptr @a, i64 3
  call void @llvm.masked.store.v2i32.p0(<2 x i32> zeroinitializer, ptr align 4 %p, <2 x i1> poison)
  ret void
}

define void @store_poison_mask_lane_constant_mem() {
; STORE-POISON-MASK-LANE-CONSTANT-MEM: Entering function: store_poison_mask_lane_constant_mem
; STORE-POISON-MASK-LANE-CONSTANT-MEM-NEXT: Stacktrace:
; STORE-POISON-MASK-LANE-CONSTANT-MEM-NEXT: #0   call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 @c, <4 x i1> <i1 false, i1 false, i1 poison, i1 false>) at @store_poison_mask_lane_constant_mem <stdin>:{{[0-9]+}}
; STORE-POISON-MASK-LANE-CONSTANT-MEM-NEXT: Immediate UB detected: Try to write to a constant memory object: ptr 0x24 [@c + 8].
; STORE-POISON-MASK-LANE-CONSTANT-MEM-NEXT: error: Execution of function 'store_poison_mask_lane_constant_mem' failed.
  call void @llvm.masked.store.v4i32.p0(<4 x i32> zeroinitializer, ptr align 4 @c, <4 x i1> <i1 false, i1 false, i1 poison, i1 false>)
  ret void
}

declare ptr @malloc(i64)
declare void @free(ptr)
