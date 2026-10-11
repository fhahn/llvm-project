; RUN: not llubi --verbose --entry-function=gather_oob < %s 2>&1 | FileCheck %s --check-prefix=GATHER-OOB
; RUN: not llubi --verbose --entry-function=gather_misaligned < %s 2>&1 | FileCheck %s --check-prefix=GATHER-MISALIGNED
; RUN: not llubi --verbose --entry-function=gather_poison_ptr < %s 2>&1 | FileCheck %s --check-prefix=GATHER-POISON-PTR
; RUN: not llubi --verbose --entry-function=gather_null < %s 2>&1 | FileCheck %s --check-prefix=GATHER-NULL
; RUN: not llubi --verbose --entry-function=gather_freed < %s 2>&1 | FileCheck %s --check-prefix=GATHER-FREED
; RUN: not llubi --verbose --entry-function=gather_poison_mask_lane_oob < %s 2>&1 | FileCheck %s --check-prefix=GATHER-POISON-MASK-LANE-OOB
; RUN: not llubi --verbose --entry-function=gather_poison_mask_lane_poison_ptr < %s 2>&1 | FileCheck %s --check-prefix=GATHER-POISON-MASK-LANE-POISON-PTR
; RUN: not llubi --verbose --entry-function=gather_wrapped_addr < %s 2>&1 | FileCheck %s --check-prefix=GATHER-WRAPPED-ADDR
; RUN: not llubi --verbose --entry-function=scatter_oob < %s 2>&1 | FileCheck %s --check-prefix=SCATTER-OOB
; RUN: not llubi --verbose --entry-function=scatter_misaligned < %s 2>&1 | FileCheck %s --check-prefix=SCATTER-MISALIGNED
; RUN: not llubi --verbose --entry-function=scatter_dead < %s 2>&1 | FileCheck %s --check-prefix=SCATTER-DEAD
; RUN: not llubi --verbose --entry-function=scatter_constant_mem < %s 2>&1 | FileCheck %s --check-prefix=SCATTER-CONSTANT-MEM
; RUN: not llubi --verbose --entry-function=scatter_poison_mask_lane_dead < %s 2>&1 | FileCheck %s --check-prefix=SCATTER-POISON-MASK-LANE-DEAD
; RUN: not llubi --verbose --entry-function=scatter_poison_mask_lane_constant_mem < %s 2>&1 | FileCheck %s --check-prefix=SCATTER-POISON-MASK-LANE-CONSTANT-MEM
; RUN: not llubi --verbose --entry-function=scatter_straddle_end < %s 2>&1 | FileCheck %s --check-prefix=SCATTER-STRADDLE-END
; RUN: not llubi --verbose --entry-function=histogram_poison_mask_lane < %s 2>&1 | FileCheck %s --check-prefix=HISTOGRAM-POISON-MASK-LANE
; RUN: not llubi --verbose --entry-function=histogram_poison_ptr_lane < %s 2>&1 | FileCheck %s --check-prefix=HISTOGRAM-POISON-PTR-LANE

@a = global [4 x i32] [i32 0, i32 1, i32 2, i32 3]
@c = constant [4 x i32] [i32 0, i32 1, i32 2, i32 3]

; Lane 2 is enabled and out of bounds; lane 3 is out of bounds, but disabled.
define void @gather_oob() {
; GATHER-OOB: Entering function: gather_oob
; GATHER-OOB-NEXT:   %ptrs = getelementptr i32, ptr @a, <4 x i64> <i64 0, i64 1, i64 4, i64 5> => { ptr 0x8 [@a], ptr 0xC [@a + 4], ptr 0x18 [@a + 16], ptr 0x1C [@a + 20] }
; GATHER-OOB-NEXT: Stacktrace:
; GATHER-OOB-NEXT: #0   %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 true, i1 false>, <4 x i32> poison) at @gather_oob <stdin>:{{[0-9]+}}
; GATHER-OOB-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 4, Address: 0x18, Object base: 0x8, Object size: 16.
; GATHER-OOB-NEXT: error: Execution of function 'gather_oob' failed.
  %ptrs = getelementptr i32, ptr @a, <4 x i64> <i64 0, i64 1, i64 4, i64 5>
  %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 true, i1 false>, <4 x i32> poison)
  ret void
}

; The align attribute makes the misaligned pointer of lane 1 poison.
define void @gather_misaligned() {
; GATHER-MISALIGNED: Entering function: gather_misaligned
; GATHER-MISALIGNED-NEXT:   %ptrs = getelementptr i8, ptr @a, <4 x i64> <i64 0, i64 2, i64 8, i64 12> => { ptr 0x8 [@a], ptr 0xA [@a + 2], ptr 0x10 [@a + 8], ptr 0x14 [@a + 12] }
; GATHER-MISALIGNED-NEXT: Stacktrace:
; GATHER-MISALIGNED-NEXT: #0   %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>, <4 x i32> poison) at @gather_misaligned <stdin>:{{[0-9]+}}
; GATHER-MISALIGNED-NEXT: Immediate UB detected: llvm.masked.gather with poison pointer lane.
; GATHER-MISALIGNED-NEXT: error: Execution of function 'gather_misaligned' failed.
  %ptrs = getelementptr i8, ptr @a, <4 x i64> <i64 0, i64 2, i64 8, i64 12>
  %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>, <4 x i32> poison)
  ret void
}

define void @gather_poison_ptr() {
; GATHER-POISON-PTR: Entering function: gather_poison_ptr
; GATHER-POISON-PTR-NEXT: Stacktrace:
; GATHER-POISON-PTR-NEXT: #0   %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 <ptr @a, ptr @a, ptr @a, ptr poison>, <4 x i1> <i1 false, i1 false, i1 false, i1 true>, <4 x i32> poison) at @gather_poison_ptr <stdin>:{{[0-9]+}}
; GATHER-POISON-PTR-NEXT: Immediate UB detected: llvm.masked.gather with poison pointer lane.
; GATHER-POISON-PTR-NEXT: error: Execution of function 'gather_poison_ptr' failed.
  %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 <ptr @a, ptr @a, ptr @a, ptr poison>, <4 x i1> <i1 false, i1 false, i1 false, i1 true>, <4 x i32> poison)
  ret void
}

define void @gather_null() {
; GATHER-NULL: Entering function: gather_null
; GATHER-NULL-NEXT: Stacktrace:
; GATHER-NULL-NEXT: #0   %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 <ptr @a, ptr null, ptr @a, ptr @a>, <4 x i1> <i1 false, i1 true, i1 false, i1 false>, <4 x i32> poison) at @gather_null <stdin>:{{[0-9]+}}
; GATHER-NULL-NEXT: Immediate UB detected: Invalid memory access via a pointer with nullary provenance.
; GATHER-NULL-NEXT: error: Execution of function 'gather_null' failed.
  %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 <ptr @a, ptr null, ptr @a, ptr @a>, <4 x i1> <i1 false, i1 true, i1 false, i1 false>, <4 x i32> poison)
  ret void
}

define void @gather_freed() {
; GATHER-FREED: Entering function: gather_freed
; GATHER-FREED-NEXT:   %p = call ptr @malloc(i64 16) => ptr 0x30 [p]
; GATHER-FREED-NEXT:   call void @free(ptr %p)
; GATHER-FREED-NEXT:   %ptrs = insertelement <4 x ptr> <ptr @a, ptr @a, ptr @a, ptr @a>, ptr %p, i64 2 => { ptr 0x8 [@a], ptr 0x8 [@a], ptr 0x30 [p (dangling)], ptr 0x8 [@a] }
; GATHER-FREED-NEXT: Stacktrace:
; GATHER-FREED-NEXT: #0   %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 false, i1 true, i1 false>, <4 x i32> poison) at @gather_freed <stdin>:{{[0-9]+}}
; GATHER-FREED-NEXT: Immediate UB detected: Try to access a dead memory object at address 0x30.
; GATHER-FREED-NEXT: error: Execution of function 'gather_freed' failed.
  %p = call ptr @malloc(i64 16)
  call void @free(ptr %p)
  %ptrs = insertelement <4 x ptr> <ptr @a, ptr @a, ptr @a, ptr @a>, ptr %p, i64 2
  %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 false, i1 true, i1 false>, <4 x i32> poison)
  ret void
}

; A poison mask lane may be enabled, so its pointer must be valid. The other
; lanes are valid.
define void @gather_poison_mask_lane_oob() {
; GATHER-POISON-MASK-LANE-OOB: Entering function: gather_poison_mask_lane_oob
; GATHER-POISON-MASK-LANE-OOB-NEXT:   %ptrs = getelementptr i32, ptr @a, <4 x i64> <i64 0, i64 4, i64 2, i64 3> => { ptr 0x8 [@a], ptr 0x18 [@a + 16], ptr 0x10 [@a + 8], ptr 0x14 [@a + 12] }
; GATHER-POISON-MASK-LANE-OOB-NEXT: Stacktrace:
; GATHER-POISON-MASK-LANE-OOB-NEXT: #0   %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 poison, i1 true, i1 false>, <4 x i32> poison) at @gather_poison_mask_lane_oob <stdin>:{{[0-9]+}}
; GATHER-POISON-MASK-LANE-OOB-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 4, Address: 0x18, Object base: 0x8, Object size: 16.
; GATHER-POISON-MASK-LANE-OOB-NEXT: error: Execution of function 'gather_poison_mask_lane_oob' failed.
  %ptrs = getelementptr i32, ptr @a, <4 x i64> <i64 0, i64 4, i64 2, i64 3>
  %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 poison, i1 true, i1 false>, <4 x i32> poison)
  ret void
}

define void @gather_poison_mask_lane_poison_ptr() {
; GATHER-POISON-MASK-LANE-POISON-PTR: Entering function: gather_poison_mask_lane_poison_ptr
; GATHER-POISON-MASK-LANE-POISON-PTR-NEXT: Stacktrace:
; GATHER-POISON-MASK-LANE-POISON-PTR-NEXT: #0   %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 <ptr @a, ptr poison, ptr @a, ptr @a>, <4 x i1> <i1 true, i1 poison, i1 false, i1 false>, <4 x i32> poison) at @gather_poison_mask_lane_poison_ptr <stdin>:{{[0-9]+}}
; GATHER-POISON-MASK-LANE-POISON-PTR-NEXT: Immediate UB detected: llvm.masked.gather with poison pointer lane.
; GATHER-POISON-MASK-LANE-POISON-PTR-NEXT: error: Execution of function 'gather_poison_mask_lane_poison_ptr' failed.
  %r = call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> align 4 <ptr @a, ptr poison, ptr @a, ptr @a>, <4 x i1> <i1 true, i1 poison, i1 false, i1 false>, <4 x i32> poison)
  ret void
}

; The address of lane 1 wraps around to the top of the address space.
define void @gather_wrapped_addr() {
; GATHER-WRAPPED-ADDR: Entering function: gather_wrapped_addr
; GATHER-WRAPPED-ADDR-NEXT:   %ptrs = getelementptr i8, ptr @a, <2 x i64> <i64 0, i64 -9> => { ptr 0x8 [@a], ptr 0xFFFFFFFFFFFFFFFF [@a + -9] }
; GATHER-WRAPPED-ADDR-NEXT: Stacktrace:
; GATHER-WRAPPED-ADDR-NEXT: #0   %r = call <2 x i8> @llvm.masked.gather.v2i8.v2p0(<2 x ptr> %ptrs, <2 x i1> splat (i1 true), <2 x i8> poison) at @gather_wrapped_addr <stdin>:{{[0-9]+}}
; GATHER-WRAPPED-ADDR-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 1, Address: 0xFFFFFFFFFFFFFFFF, Object base: 0x8, Object size: 16.
; GATHER-WRAPPED-ADDR-NEXT: error: Execution of function 'gather_wrapped_addr' failed.
  %ptrs = getelementptr i8, ptr @a, <2 x i64> <i64 0, i64 -9>
  %r = call <2 x i8> @llvm.masked.gather.v2i8.v2p0(<2 x ptr> %ptrs, <2 x i1> splat (i1 true), <2 x i8> poison)
  ret void
}

; Lane 0 accesses the last 4 bytes of @a; lane 1 extends one byte past its end.
define void @scatter_straddle_end() {
; SCATTER-STRADDLE-END: Entering function: scatter_straddle_end
; SCATTER-STRADDLE-END-NEXT:   %ptrs = getelementptr i8, ptr @a, <2 x i64> <i64 12, i64 13> => { ptr 0x14 [@a + 12], ptr 0x15 [@a + 13] }
; SCATTER-STRADDLE-END-NEXT: Stacktrace:
; SCATTER-STRADDLE-END-NEXT: #0   call void @llvm.masked.scatter.v2i32.v2p0(<2 x i32> splat (i32 1), <2 x ptr> %ptrs, <2 x i1> splat (i1 true)) at @scatter_straddle_end <stdin>:{{[0-9]+}}
; SCATTER-STRADDLE-END-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 4, Address: 0x15, Object base: 0x8, Object size: 16.
; SCATTER-STRADDLE-END-NEXT: error: Execution of function 'scatter_straddle_end' failed.
  %ptrs = getelementptr i8, ptr @a, <2 x i64> <i64 12, i64 13>
  call void @llvm.masked.scatter.v2i32.v2p0(<2 x i32> splat (i32 1), <2 x ptr> %ptrs, <2 x i1> splat (i1 true))
  ret void
}

; Lane 1 is enabled and before the start of the object.
define void @scatter_oob() {
; SCATTER-OOB: Entering function: scatter_oob
; SCATTER-OOB-NEXT:   %ptrs = getelementptr i32, ptr @a, <4 x i64> <i64 0, i64 -1, i64 2, i64 3> => { ptr 0x8 [@a], ptr 0x4 [@a + -4], ptr 0x10 [@a + 8], ptr 0x14 [@a + 12] }
; SCATTER-OOB-NEXT: Stacktrace:
; SCATTER-OOB-NEXT: #0   call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>) at @scatter_oob <stdin>:{{[0-9]+}}
; SCATTER-OOB-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 4, Address: 0x4, Object base: 0x8, Object size: 16.
; SCATTER-OOB-NEXT: error: Execution of function 'scatter_oob' failed.
  %ptrs = getelementptr i32, ptr @a, <4 x i64> <i64 0, i64 -1, i64 2, i64 3>
  call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>)
  ret void
}

define void @scatter_misaligned() {
; SCATTER-MISALIGNED: Entering function: scatter_misaligned
; SCATTER-MISALIGNED-NEXT:   %ptrs = getelementptr i8, ptr @a, <4 x i64> <i64 0, i64 2, i64 8, i64 12> => { ptr 0x8 [@a], ptr 0xA [@a + 2], ptr 0x10 [@a + 8], ptr 0x14 [@a + 12] }
; SCATTER-MISALIGNED-NEXT: Stacktrace:
; SCATTER-MISALIGNED-NEXT: #0   call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>) at @scatter_misaligned <stdin>:{{[0-9]+}}
; SCATTER-MISALIGNED-NEXT: Immediate UB detected: llvm.masked.scatter with poison pointer lane.
; SCATTER-MISALIGNED-NEXT: error: Execution of function 'scatter_misaligned' failed.
  %ptrs = getelementptr i8, ptr @a, <4 x i64> <i64 0, i64 2, i64 8, i64 12>
  call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>)
  ret void
}

define void @scatter_dead() {
; SCATTER-DEAD: Entering function: scatter_dead
; SCATTER-DEAD-NEXT:   %s = alloca i32, align 4 => ptr 0x30 [s (dead)]
; SCATTER-DEAD-NEXT:   call void @llvm.lifetime.start.p0(ptr %s)
; SCATTER-DEAD-NEXT:   call void @llvm.lifetime.end.p0(ptr %s)
; SCATTER-DEAD-NEXT:   %ptrs = insertelement <4 x ptr> <ptr @a, ptr @a, ptr @a, ptr @a>, ptr %s, i64 1 => { ptr 0x8 [@a], ptr 0x30 [s (dead)], ptr 0x8 [@a], ptr 0x8 [@a] }
; SCATTER-DEAD-NEXT: Stacktrace:
; SCATTER-DEAD-NEXT: #0   call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>) at @scatter_dead <stdin>:{{[0-9]+}}
; SCATTER-DEAD-NEXT: Immediate UB detected: Try to access a dead memory object at address 0x30.
; SCATTER-DEAD-NEXT: error: Execution of function 'scatter_dead' failed.
  %s = alloca i32, align 4
  call void @llvm.lifetime.start.p0(ptr %s)
  call void @llvm.lifetime.end.p0(ptr %s)
  %ptrs = insertelement <4 x ptr> <ptr @a, ptr @a, ptr @a, ptr @a>, ptr %s, i64 1
  call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 true, i1 false, i1 false>)
  ret void
}

define void @scatter_constant_mem() {
; SCATTER-CONSTANT-MEM: Entering function: scatter_constant_mem
; SCATTER-CONSTANT-MEM-NEXT: Stacktrace:
; SCATTER-CONSTANT-MEM-NEXT: #0   call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 <ptr @a, ptr @a, ptr @a, ptr @c>, <4 x i1> <i1 true, i1 false, i1 false, i1 true>) at @scatter_constant_mem <stdin>:{{[0-9]+}}
; SCATTER-CONSTANT-MEM-NEXT: Immediate UB detected: Try to write to a constant memory object: ptr 0x1C [@c].
; SCATTER-CONSTANT-MEM-NEXT: error: Execution of function 'scatter_constant_mem' failed.
  call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 <ptr @a, ptr @a, ptr @a, ptr @c>, <4 x i1> <i1 true, i1 false, i1 false, i1 true>)
  ret void
}

define void @scatter_poison_mask_lane_dead() {
; SCATTER-POISON-MASK-LANE-DEAD: Entering function: scatter_poison_mask_lane_dead
; SCATTER-POISON-MASK-LANE-DEAD-NEXT:   %s = alloca i32, align 4 => ptr 0x30 [s (dead)]
; SCATTER-POISON-MASK-LANE-DEAD-NEXT:   call void @llvm.lifetime.start.p0(ptr %s)
; SCATTER-POISON-MASK-LANE-DEAD-NEXT:   call void @llvm.lifetime.end.p0(ptr %s)
; SCATTER-POISON-MASK-LANE-DEAD-NEXT:   %ptrs = insertelement <4 x ptr> <ptr @a, ptr @a, ptr @a, ptr @a>, ptr %s, i64 2 => { ptr 0x8 [@a], ptr 0x8 [@a], ptr 0x30 [s (dead)], ptr 0x8 [@a] }
; SCATTER-POISON-MASK-LANE-DEAD-NEXT: Stacktrace:
; SCATTER-POISON-MASK-LANE-DEAD-NEXT: #0   call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 false, i1 poison, i1 false>) at @scatter_poison_mask_lane_dead <stdin>:{{[0-9]+}}
; SCATTER-POISON-MASK-LANE-DEAD-NEXT: Immediate UB detected: Try to access a dead memory object at address 0x30.
; SCATTER-POISON-MASK-LANE-DEAD-NEXT: error: Execution of function 'scatter_poison_mask_lane_dead' failed.
  %s = alloca i32, align 4
  call void @llvm.lifetime.start.p0(ptr %s)
  call void @llvm.lifetime.end.p0(ptr %s)
  %ptrs = insertelement <4 x ptr> <ptr @a, ptr @a, ptr @a, ptr @a>, ptr %s, i64 2
  call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 %ptrs, <4 x i1> <i1 true, i1 false, i1 poison, i1 false>)
  ret void
}

define void @scatter_poison_mask_lane_constant_mem() {
; SCATTER-POISON-MASK-LANE-CONSTANT-MEM: Entering function: scatter_poison_mask_lane_constant_mem
; SCATTER-POISON-MASK-LANE-CONSTANT-MEM-NEXT: Stacktrace:
; SCATTER-POISON-MASK-LANE-CONSTANT-MEM-NEXT: #0   call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 <ptr @a, ptr @c, ptr @a, ptr @a>, <4 x i1> <i1 false, i1 poison, i1 true, i1 false>) at @scatter_poison_mask_lane_constant_mem <stdin>:{{[0-9]+}}
; SCATTER-POISON-MASK-LANE-CONSTANT-MEM-NEXT: Immediate UB detected: Try to write to a constant memory object: ptr 0x1C [@c].
; SCATTER-POISON-MASK-LANE-CONSTANT-MEM-NEXT: error: Execution of function 'scatter_poison_mask_lane_constant_mem' failed.
  call void @llvm.masked.scatter.v4i32.v4p0(<4 x i32> splat (i32 1), <4 x ptr> align 4 <ptr @a, ptr @c, ptr @a, ptr @a>, <4 x i1> <i1 false, i1 poison, i1 true, i1 false>)
  ret void
}

declare ptr @malloc(i64)
declare void @free(ptr)

; Unlike gather/scatter, the histogram intrinsics treat poison mask lanes as UB.
define void @histogram_poison_mask_lane() {
; HISTOGRAM-POISON-MASK-LANE: Entering function: histogram_poison_mask_lane
; HISTOGRAM-POISON-MASK-LANE-NEXT: Stacktrace:
; HISTOGRAM-POISON-MASK-LANE-NEXT: #0   call void @llvm.experimental.vector.histogram.add.v2p0.i32(<2 x ptr> <ptr @a, ptr @a>, i32 1, <2 x i1> <i1 true, i1 poison>) at @histogram_poison_mask_lane <stdin>:{{[0-9]+}}
; HISTOGRAM-POISON-MASK-LANE-NEXT: Immediate UB detected: experimental vector histogram intrinsic with poison mask lane.
; HISTOGRAM-POISON-MASK-LANE-NEXT: error: Execution of function 'histogram_poison_mask_lane' failed.
  call void @llvm.experimental.vector.histogram.add.v2p0.i32(<2 x ptr> <ptr @a, ptr @a>, i32 1, <2 x i1> <i1 true, i1 poison>)
  ret void
}

define void @histogram_poison_ptr_lane() {
; HISTOGRAM-POISON-PTR-LANE: Entering function: histogram_poison_ptr_lane
; HISTOGRAM-POISON-PTR-LANE-NEXT: Stacktrace:
; HISTOGRAM-POISON-PTR-LANE-NEXT: #0   call void @llvm.experimental.vector.histogram.add.v2p0.i32(<2 x ptr> <ptr @a, ptr poison>, i32 1, <2 x i1> splat (i1 true)) at @histogram_poison_ptr_lane <stdin>:{{[0-9]+}}
; HISTOGRAM-POISON-PTR-LANE-NEXT: Immediate UB detected: experimental vector histogram intrinsic with poison pointer lane.
; HISTOGRAM-POISON-PTR-LANE-NEXT: error: Execution of function 'histogram_poison_ptr_lane' failed.
  call void @llvm.experimental.vector.histogram.add.v2p0.i32(<2 x ptr> <ptr @a, ptr poison>, i32 1, <2 x i1> splat (i1 true))
  ret void
}
