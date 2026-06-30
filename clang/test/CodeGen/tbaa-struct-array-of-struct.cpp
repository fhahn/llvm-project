// RUN: %clang_cc1 -triple x86_64-linux -O1 %s \
// RUN:     -emit-llvm -o - | FileCheck -check-prefixes=CHECK,CHECK-OLD %s
// RUN: %clang_cc1 -triple x86_64-linux -O1 %s \
// RUN:     -new-struct-path-tbaa -emit-llvm -o - | \
// RUN:     FileCheck -check-prefixes=CHECK,CHECK-NEW %s
//
// Check that a struct member that is an array of an aggregate type produces a
// well-formed !tbaa.struct field descriptor.
//
// In the default (non-new) struct-path format an array maps to its element
// type; if that element is an aggregate this would yield a struct type node
// used as a scalar access type, which is invalid IR and crashes the backend
// once SROA splits the aggregate copy ("Access type node must be a valid
// scalar type" / "Broken function found"). Such array members fall back to the
// char access type instead, as we already do for unions and bitfields. The new
// struct-path format describes them with a full struct-path tag and is
// unaffected.

struct Inner {
  long x;
  long y;
};

struct Outer {
  Inner arr[2];
  int tail;
};

// CHECK-LABEL: define {{.*}}void @_Z4copyP5OuterS0_
// CHECK: call void @llvm.memcpy{{.*}}, !tbaa.struct [[TS:![0-9]+]]
void copy(Outer *a, Outer *b) { *a = *b; }

// Default format: the array-of-aggregate member (32 bytes at offset 0) is
// described by a single field whose access type is the char node, never a
// struct node.
// CHECK-OLD-DAG: [[TS]] = !{i64 0, i64 32, [[TAG_CHAR:![0-9]+]], i64 32, i64 4, {{![0-9]+}}}
// CHECK-OLD-DAG: [[TAG_CHAR]] = !{[[TYPE_CHAR:![0-9]+]], [[TYPE_CHAR]], i64 0}
// CHECK-OLD-DAG: [[TYPE_CHAR]] = !{!"omnipotent char", {{![0-9]+}}, i64 0}

// New format: the member carries a struct-path access tag referencing the
// element type Inner; this is valid in this format.
// CHECK-NEW-DAG: [[TS]] = !{i64 0, i64 32, [[TAG_INNER:![0-9]+]], i64 32, i64 4, {{![0-9]+}}}
// CHECK-NEW-DAG: [[TAG_INNER]] = !{[[TYPE_INNER:![0-9]+]], [[TYPE_INNER]], i64 0, i64 32}
// CHECK-NEW-DAG: [[TYPE_INNER]] = !{{{![0-9]+}}, i64 16, !"_ZTS5Inner", {{.*}}}
