// RUN: %clang_cc1 %s  -emit-llvm -o - | FileCheck %s

// This was a problem in Sema, but only shows up as noinline missing
// in CodeGen.

// CHECK: define linkonce_odr void @_ZN6VectorIiE13growStorageByEv(%struct.Vector* %this) [[NI:#[0-9]+]]

template <class Ty> struct Vector  {
  void growStorageBy();
};
template <class T> __attribute__((noinline)) void Vector<T>::growStorageBy() {
}
void foo() {
 Vector<int> strs;
 strs.growStorageBy();
}

// CHECK: attributes [[NI]] = { noinline nounwind{{.*}} }
