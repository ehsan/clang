// RUN: %clang_cc1 -verify -fsyntax-only -Wshadow %s
// RUN: %clang_cc1 -verify -fsyntax-only -Wshadow-field %s

struct Base {
  int m; // expected-note {{previous declaration is here}}
  struct Inner {
    int m;
  };
};
struct Derived : Base {
  struct Inner {
    int m;
  };
private:
  int m; // expected-warning {{declaration shadows a base class member}} expected-note {{previous declaration is here}}
};
class MoreDerived : Derived {
  int m; // expected-warning {{declaration shadows a base class member}}
};
union U {
  int m;
};
