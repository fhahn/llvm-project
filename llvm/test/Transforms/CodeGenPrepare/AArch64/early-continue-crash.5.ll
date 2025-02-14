target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

define i32 @main(i64 %newLength.044) personality ptr null {
entry:
  br label %for.body

for.body:                                         ; preds = %invoke.cont6, %entry
  %newLength.0441 = phi i64 [ %add, %invoke.cont6 ], [ 0, %entry ]
  %capacity.043 = phi i64 [ %capacity.1, %invoke.cont6 ], [ 0, %entry ]
  %cmp3 = icmp ugt i64 %newLength.0441, %capacity.043
  br i1 %cmp3, label %if.then, label %if.end

if.then:                                          ; preds = %for.body
  invoke void @_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm(ptr null, i64 0)
          to label %if.end unwind label %lpad4.loopexit

lpad4.loopexit:                                   ; preds = %if.end, %if.then
  %lpad.loopexit = landingpad { ptr, i32 }
          cleanup
  ret i32 0

if.end:                                           ; preds = %if.then, %for.body
  %capacity.1 = phi i64 [ 0, %if.then ], [ 1, %for.body ]
  %call.i26 = invoke ptr @_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc(ptr null, ptr null)
          to label %invoke.cont6 unwind label %lpad4.loopexit

invoke.cont6:                                     ; preds = %if.end
  %add = or i64 %newLength.044, 1
  br label %for.body
}

declare void @_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm()

declare ptr @_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc()

; uselistorder directives
uselistorder ptr null, { 1, 2, 3, 4, 5, 0 }
