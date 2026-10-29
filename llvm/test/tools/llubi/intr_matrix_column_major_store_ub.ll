; RUN: not llubi --verbose --entry-function=out_of_bounds < %s 2>&1 | FileCheck %s --check-prefix=OOB
; RUN: not llubi --verbose --entry-function=stride_less_than_rows < %s 2>&1 | FileCheck %s --check-prefix=STRIDE
; RUN: not llubi --verbose --entry-function=poison_stride < %s 2>&1 | FileCheck %s --check-prefix=POISON-STRIDE
; RUN: not llubi --verbose --entry-function=misaligned < %s 2>&1 | FileCheck %s --check-prefix=MISALIGNED
; RUN: not llubi --verbose --entry-function=wrapping_offset < %s 2>&1 | FileCheck %s --check-prefix=WRAP

@a = global [8 x i32] [i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7]

define void @out_of_bounds() {
; OOB: Entering function: out_of_bounds
; OOB-NEXT:   %p = getelementptr i32, ptr @a, i64 4 => ptr 0x20 [@a + 16]
; OOB-NEXT: Stacktrace:
; OOB-NEXT: #0   call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr %p, i64 3, i1 false, i32 2, i32 2) at @out_of_bounds <stdin>:{{[0-9]+}}
; OOB-NEXT: Immediate UB detected: Memory access is out of bounds. Accessed size: 8, Address: 0x2C, Object base: 0x10, Object size: 32.
; OOB-NEXT: error: Execution of function 'out_of_bounds' failed.
  %p = getelementptr i32, ptr @a, i64 4
  call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr %p, i64 3, i1 false, i32 2, i32 2)
  ret void
}

define void @stride_less_than_rows() {
; STRIDE: Entering function: stride_less_than_rows
; STRIDE-NEXT: Stacktrace:
; STRIDE-NEXT: #0   call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr @a, i64 1, i1 false, i32 2, i32 2) at @stride_less_than_rows <stdin>:{{[0-9]+}}
; STRIDE-NEXT: Immediate UB detected: llvm.matrix.column.major.store with stride 1 less than the number of rows 2.
; STRIDE-NEXT: error: Execution of function 'stride_less_than_rows' failed.
  call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr @a, i64 1, i1 false, i32 2, i32 2)
  ret void
}

define void @poison_stride() {
; POISON-STRIDE: Entering function: poison_stride
; POISON-STRIDE-NEXT: Stacktrace:
; POISON-STRIDE-NEXT: #0   call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr @a, i64 poison, i1 false, i32 2, i32 2) at @poison_stride <stdin>:{{[0-9]+}}
; POISON-STRIDE-NEXT: Immediate UB detected: llvm.matrix.column.major.store with poison stride.
; POISON-STRIDE-NEXT: error: Execution of function 'poison_stride' failed.
  call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr @a, i64 poison, i1 false, i32 2, i32 2)
  ret void
}

; The align attribute turns the misaligned pointer into poison.
define void @misaligned() {
; MISALIGNED: Entering function: misaligned
; MISALIGNED-NEXT:   %p = getelementptr i32, ptr @a, i64 1 => ptr 0x14 [@a + 4]
; MISALIGNED-NEXT: Stacktrace:
; MISALIGNED-NEXT: #0   call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr align 8 %p, i64 2, i1 false, i32 2, i32 2) at @misaligned <stdin>:{{[0-9]+}}
; MISALIGNED-NEXT: Immediate UB detected: Invalid memory access with a poison pointer.
; MISALIGNED-NEXT: error: Execution of function 'misaligned' failed.
  %p = getelementptr i32, ptr @a, i64 1
  call void @llvm.matrix.column.major.store.v4i32.i64(<4 x i32> zeroinitializer, ptr align 8 %p, i64 2, i1 false, i32 2, i32 2)
  ret void
}

; The stride is unsigned, so column 1 does not start 4 bytes before %p and the
; inbounds nuw GEP is poison.
define void @wrapping_offset() {
; WRAP: Entering function: wrapping_offset
; WRAP-NEXT:   %p = getelementptr i32, ptr @a, i64 1 => ptr 0x14 [@a + 4]
; WRAP-NEXT: Stacktrace:
; WRAP-NEXT: #0   call void @llvm.matrix.column.major.store.v2i32.i64(<2 x i32> zeroinitializer, ptr %p, i64 -1, i1 false, i32 1, i32 2) at @wrapping_offset <stdin>:{{[0-9]+}}
; WRAP-NEXT: Immediate UB detected: Invalid memory access with a poison pointer.
; WRAP-NEXT: error: Execution of function 'wrapping_offset' failed.
  %p = getelementptr i32, ptr @a, i64 1
  call void @llvm.matrix.column.major.store.v2i32.i64(<2 x i32> zeroinitializer, ptr %p, i64 -1, i1 false, i32 1, i32 2)
  ret void
}
