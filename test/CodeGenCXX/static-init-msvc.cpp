// RUN: %clang_cc1 -triple i686-windows-msvc -fms-extensions -emit-llvm -O0 -w -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-windows-msvc -fms-extensions -emit-llvm -O0 -w -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple i686-windows-gnu -fms-extensions -emit-llvm -O0 -w -o - %s | FileCheck %s --check-prefix GNU
// RUN: %clang_cc1 -triple x86_64-windows-gnu -fms-extensions -emit-llvm -O0 -w -o - %s | FileCheck %s --check-prefix GNU

void fun_and() {
// CHECK-LABEL: @"\01?fun_and@@YAXXZ"()
// GNU-LABEL: @_Z7fun_andv()
  static int k;
  static const int foo = 0 && k;
// CHECK: init:
// CHECK-NEXT: store i32 0, i32* @"\01?foo@?1??fun_and@@YAXXZ@4HB", align 4
// GNU: entry:
// GNU-NEXT: ret void
}

void fun_or() {
// CHECK-LABEL: @"\01?fun_or@@YAXXZ"()
// GNU-LABEL: @_Z6fun_orv()
  static int k;
  static const int foo = 1 || k;
// CHECK: init:
// CHECK-NEXT: store i32 1, i32* @"\01?foo@?1??fun_or@@YAXXZ@4HB", align 4
// GNU: entry:
// GNU-NEXT: ret void
}

void fun_comma() {
// CHECK-LABEL: @"\01?fun_comma@@YAXXZ"()
// GNU-LABEL: @_Z9fun_commav()
  static int k;
  static const int foo = (k, 0);
// CHECK: init:
// CHECK-NEXT: store i32 0, i32* @"\01?foo@?1??fun_comma@@YAXXZ@4HB", align 4
// GNU: entry:
// GNU-NEXT: ret void
}

void fun_cond() {
// CHECK-LABEL: @"\01?fun_cond@@YAXXZ"()
// GNU-LABEL: @_Z8fun_condv()
  static int k;
  static const int foo = true ? 0 : k;
// CHECK: init:
// CHECK-NEXT: store i32 0, i32* @"\01?foo@?1??fun_cond@@YAXXZ@4HB", align 4
// GNU: entry:
// GNU-NEXT: ret void
}
