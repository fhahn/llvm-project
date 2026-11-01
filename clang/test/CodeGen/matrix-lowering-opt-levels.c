// RUN: %clang -O0 -fenable-matrix -S -emit-llvm %s -o - | FileCheck --check-prefix=O0 %s
// RUN: %clang -O1 -fenable-matrix -S -emit-llvm %s -o - | FileCheck  %s
// RUN: %clang -O2 -fenable-matrix -S -emit-llvm %s -o - | FileCheck  %s
// RUN: %clang -O3 -fenable-matrix -S -emit-llvm %s -o - | FileCheck  %s
// RUN: %clang -Ofast -fenable-matrix -S -emit-llvm %s -o - | FileCheck  %s
// RUN: %clang -Os -fenable-matrix -S -emit-llvm %s -o - | FileCheck  %s
// RUN: %clang -Oz -fenable-matrix -S -emit-llvm %s -o - | FileCheck  %s

// Smoke test that the matrix intrinsics are lowered at -O1 and above, and left
// to the backend at -O0.

typedef float m4x4_t __attribute__((matrix_type(4, 4)));

m4x4_t f(m4x4_t a, m4x4_t b, m4x4_t c) {
  //
  // CHECK-LABEL: f(
  // CHECK-NOT:     @llvm.matrix
  // CHECK:       }
  //
  // O0-LABEL: f(
  // O0:         @llvm.matrix.multiply
  // O0:       }
  //
  return a + b * c;
}
