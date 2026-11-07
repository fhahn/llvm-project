; RUN: opt < %s -passes='cg-profile' -S | FileCheck %s

; A function whose entire name is a strippable suffix (e.g. ".llvm.123")
; canonicalizes to an empty name. The symbol table must still have the
; functions after it: the indirect call in @f resolves to @f.

; CHECK: define void @.llvm.123()
define void @.llvm.123() {
  ret void
}

define void @f(ptr %p) !prof !0 {
  call void %p(), !prof !1
  ret void
}

!0 = !{!"function_entry_count", i64 1}
!1 = !{!"VP", i32 0, i64 1, i64 -3706093650706652785, i64 1}

; CHECK: !{ptr @f, ptr @f, i64 1}
