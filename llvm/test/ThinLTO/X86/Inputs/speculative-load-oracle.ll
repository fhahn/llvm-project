target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <4 x i32> @f(ptr, i64)

define <4 x i32> @g(ptr %p) {
  %r = call <4 x i32> @f(ptr %p, i64 4)
  ret <4 x i32> %r
}
