// RUN: %clang_cc1 -std=c++14 -fsyntax-only -verify %s
// RUN: %clang_cc1 -std=c++14 -ast-dump %s | FileCheck %s

// Overload resolution for a builtin operator computes the conversions of the
// arguments for each builtin candidate. Binding a reference skips a conversion
// function with an undeduced return type, but other conversions deduce it, so
// the conversions of later candidates must see the deduced conversion
// function.

// expected-no-diagnostics

namespace stale {
template <class T> struct S {
  T m;
  int *p;
  constexpr operator decltype(auto)() { return (m); }
  constexpr operator int &() const { return *p; }
};

constexpr int f() {
  int g = 0;
  S<int> s{1, &g};
  s += s;
  return s.m * 10 + g;
}
static_assert(f() == 20, "");

// CHECK-LABEL: FunctionDecl {{.*}} f 'int ()'
// CHECK:       CompoundAssignOperator {{.*}} 'int' lvalue '+='
// CHECK-NEXT:  ImplicitCastExpr {{.*}} <UserDefinedConversion>
// CHECK-NEXT:  CXXMemberCallExpr {{.*}} 'int' lvalue
// CHECK-NEXT:  MemberExpr {{.*}} .operator decltype(auto)
} // namespace stale

namespace ambiguous {
int g;
template <class T> struct S {
  T m;
  operator decltype(auto)() { return (m); }
  operator int &() const { return g; }
  operator int &() volatile { return g; }
};

void f() {
  S<int> s{1};
  s += s;
}

// CHECK-LABEL: FunctionDecl {{.*}} f 'void ()'
// CHECK:       CompoundAssignOperator {{.*}} 'int' lvalue '+='
// CHECK-NEXT:  ImplicitCastExpr {{.*}} <UserDefinedConversion>
// CHECK-NEXT:  CXXMemberCallExpr {{.*}} 'int' lvalue
// CHECK-NEXT:  MemberExpr {{.*}} .operator decltype(auto)
} // namespace ambiguous
