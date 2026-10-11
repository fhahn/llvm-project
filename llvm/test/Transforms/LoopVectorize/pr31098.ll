; REQUIRES: asserts
; RUN: opt -S -passes=loop-vectorize -force-vector-width=4 -force-vector-interleave=1 -enable-interleaved-mem-accesses=true -debug-only=loop-accesses < %s -o %t 2>&1 | FileCheck %s --check-prefix=DEBUG
; RUN: FileCheck %s --check-prefix=IR < %t

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

; The dependence distances are 8*D, 4+8*D, -4+8*D, and their negatives.
; These expressions can wrap for unrestricted D, so comparing their modular
; differences with 8*(D-1) does not prove independence. Runtime checks still
; allow vectorization. Test_bounded below retains the static proof when the
; unsigned count and byte-distance arithmetic are bounded.

; #include <stdlib.h>
; class Complex {
; private:
;   float real_;
;   float imaginary_;
;
; public:
;   Complex() : real_(0), imaginary_(0) { }
;   Complex(float real, float imaginary) : real_(real), imaginary_(imaginary) { }
;   Complex(const Complex &rhs) : real_(rhs.real()), imaginary_(rhs.imaginary()) { }
; 
;   inline float real() const { return real_; }
;   inline float imaginary() const { return imaginary_; }
; 
;   Complex operator+(const Complex& rhs) const
;   {
;    return Complex(real_ + rhs.real_, imaginary_ + rhs.imaginary_);
;   }
;
;   Complex operator-(const Complex& rhs) const
;  {
;     return Complex(real_ - rhs.real_, imaginary_ - rhs.imaginary_);
;   }
; };
;
; void Test(Complex *out, size_t size)
; {
;     size_t D = size / 2;
;     for (size_t offset = 0; offset < D; ++offset)
;     {
;         Complex t0 = out[offset];
;         Complex t1 = out[offset + D];
;         out[offset] = t1 + t0;
;         out[offset + D] = t0 - t1;
;     }
; }

; DEBUG-LABEL: LAA: Checking a loop in 'Test'
; DEBUG: LAA: No unsafe dependent memory operations in loop.  We need runtime memory checks.
; IR-LABEL: define void @Test(
; IR: vector.body:
; IR: <4 x i32>

%class.Complex = type { float, float }

define void @Test(ptr nocapture %out, i64 %size) {
entry:
  %div = lshr i64 %size, 1
  %cmp47 = icmp eq i64 %div, 0
  br i1 %cmp47, label %for.cond.cleanup, label %for.body.preheader

for.body.preheader:
  br label %for.body

for.cond.cleanup.loopexit:
  br label %for.cond.cleanup

for.cond.cleanup:
  ret void

for.body:
  %offset.048 = phi i64 [ %inc, %for.body ], [ 0, %for.body.preheader ]
  %0 = getelementptr inbounds %class.Complex, ptr %out, i64 %offset.048, i32 0
  %1 = load float, ptr %0, align 4
  %imaginary_.i.i = getelementptr inbounds %class.Complex, ptr %out, i64 %offset.048, i32 1
  %2 = load float, ptr %imaginary_.i.i, align 4
  %add = add nuw i64 %offset.048, %div
  %3 = getelementptr inbounds %class.Complex, ptr %out, i64 %add, i32 0
  %4 = load float, ptr %3, align 4
  %imaginary_.i.i28 = getelementptr inbounds %class.Complex, ptr %out, i64 %add, i32 1
  %5 = load float, ptr %imaginary_.i.i28, align 4
  %add.i = fadd fast float %4, %1
  %add4.i = fadd fast float %5, %2
  store float %add.i, ptr %0, align 4
  store float %add4.i, ptr %imaginary_.i.i, align 4
  %sub.i = fsub fast float %1, %4
  %sub4.i = fsub fast float %2, %5
  store float %sub.i, ptr %3, align 4
  store float %sub4.i, ptr %imaginary_.i.i28, align 4
  %inc = add nuw nsw i64 %offset.048, 1
  %exitcond = icmp eq i64 %inc, %div
  br i1 %exitcond, label %for.cond.cleanup.loopexit, label %for.body
}

; A positive D <= 2^59 keeps the count, byte stride product, and access-size
; addition in range, while preserving all six symbolic distance expressions.
; DEBUG-LABEL: LAA: Checking a loop in 'Test_bounded'
; DEBUG: LAA: No unsafe dependent memory operations in loop.  We don't need runtime memory checks.
; IR-LABEL: define void @Test_bounded(
; IR: vector.body:
; IR: <4 x i32>

define void @Test_bounded(ptr nocapture %out, i64 %size) {
entry:
  %shr = lshr i64 %size, 5
  %div = add nuw nsw i64 %shr, 1
  %cmp47 = icmp eq i64 %div, 0
  br i1 %cmp47, label %for.cond.cleanup, label %for.body.preheader

for.body.preheader:
  br label %for.body

for.cond.cleanup.loopexit:
  br label %for.cond.cleanup

for.cond.cleanup:
  ret void

for.body:
  %offset.048 = phi i64 [ %inc, %for.body ], [ 0, %for.body.preheader ]
  %0 = getelementptr inbounds %class.Complex, ptr %out, i64 %offset.048, i32 0
  %1 = load float, ptr %0, align 4
  %imaginary_.i.i = getelementptr inbounds %class.Complex, ptr %out, i64 %offset.048, i32 1
  %2 = load float, ptr %imaginary_.i.i, align 4
  %add = add nuw i64 %offset.048, %div
  %3 = getelementptr inbounds %class.Complex, ptr %out, i64 %add, i32 0
  %4 = load float, ptr %3, align 4
  %imaginary_.i.i28 = getelementptr inbounds %class.Complex, ptr %out, i64 %add, i32 1
  %5 = load float, ptr %imaginary_.i.i28, align 4
  %add.i = fadd fast float %4, %1
  %add4.i = fadd fast float %5, %2
  store float %add.i, ptr %0, align 4
  store float %add4.i, ptr %imaginary_.i.i, align 4
  %sub.i = fsub fast float %1, %4
  %sub4.i = fsub fast float %2, %5
  store float %sub.i, ptr %3, align 4
  store float %sub4.i, ptr %imaginary_.i.i28, align 4
  %inc = add nuw nsw i64 %offset.048, 1
  %exitcond = icmp eq i64 %inc, %div
  br i1 %exitcond, label %for.cond.cleanup.loopexit, label %for.body
}
