; RUN: opt -S -disable-output "-passes=print<scalar-evolution><use-context>,print<scalar-evolution>" %s 2>&1 | FileCheck %s --check-prefix=CTX-THEN-NOCTX
; RUN: opt -S -disable-output "-passes=print<scalar-evolution><use-context>,print<scalar-evolution><use-context>" %s 2>&1 | FileCheck %s --check-prefix=CTX-THEN-CTX
; RUN: opt -S -disable-output "-passes=print<scalar-evolution>,print<scalar-evolution><use-context>" %s 2>&1 | FileCheck %s --check-prefix=NOCTX-FIRST
; RUN: opt -S -disable-output "-passes=print<scalar-evolution>,print<scalar-evolution>" %s 2>&1 | FileCheck %s --check-prefix=NOCTX-FIRST

define void @f(ptr %base, i32 %n) {
  %ext = zext i32 %n to i64
  %gep = getelementptr inbounds i8, ptr %base, i64 %ext
  ret void
}

; use-context first: shows (u nuw). Without context second: strips it.
; CTX-THEN-NOCTX-LABEL: 'f'
; CTX-THEN-NOCTX:         %gep = getelementptr inbounds i8, ptr %base, i64 %ext
; CTX-THEN-NOCTX-NEXT:    -->  ((zext i32 %n to i64) + %base)(u nuw) U:
; CTX-THEN-NOCTX:       'f'
; CTX-THEN-NOCTX:         %gep = getelementptr inbounds i8, ptr %base, i64 %ext
; CTX-THEN-NOCTX-NEXT:    -->  ((zext i32 %n to i64) + %base) U:

; use-context both times: caches with flags, second returns them from cache.
; CTX-THEN-CTX-LABEL: 'f'
; CTX-THEN-CTX:         %gep = getelementptr inbounds i8, ptr %base, i64 %ext
; CTX-THEN-CTX-NEXT:    -->  ((zext i32 %n to i64) + %base)(u nuw) U:
; CTX-THEN-CTX:       'f'
; CTX-THEN-CTX:         %gep = getelementptr inbounds i8, ptr %base, i64 %ext
; CTX-THEN-CTX-NEXT:    -->  ((zext i32 %n to i64) + %base)(u nuw) U:

; Without context first: no use-specific flags in either print.
; NOCTX-FIRST-LABEL: 'f'
; NOCTX-FIRST:         %gep = getelementptr inbounds i8, ptr %base, i64 %ext
; NOCTX-FIRST-NEXT:    -->  ((zext i32 %n to i64) + %base) U:
; NOCTX-FIRST:       'f'
; NOCTX-FIRST:         %gep = getelementptr inbounds i8, ptr %base, i64 %ext
; NOCTX-FIRST-NEXT:    -->  ((zext i32 %n to i64) + %base) U:
