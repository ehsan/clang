// RUN: %clang_cc1 -analyze -analyzer-checker=core,unix.Malloc,debug.ExprInspection -analyzer-store region -verify %s

void clang_analyzer_eval(bool);

typedef typeof(sizeof(int)) size_t;
extern "C" void *malloc(size_t);

// This is the standard placement new.
inline void* operator new(size_t, void* __p) throw()
{
  return __p;
}

void *testPlacementNew() {
  int *x = (int *)malloc(sizeof(int));
  *x = 1;
  clang_analyzer_eval(*x == 1); // expected-warning{{TRUE}};

  void *y = new (x) int;
  clang_analyzer_eval(x == y); // expected-warning{{TRUE}};
  clang_analyzer_eval(*x == 1); // expected-warning{{UNKNOWN}};

  return y;
}

void *operator new(size_t, size_t, int *);
void *testCustomNew() {
  int x[1] = {1};
  clang_analyzer_eval(*x == 1); // expected-warning{{TRUE}};

  void *y = new (0, x) int;
  clang_analyzer_eval(*x == 1); // expected-warning{{UNKNOWN}};

  return y; // no-warning
}


//--------------------------------
// Incorrectly-modelled behavior
//--------------------------------

void testZeroInitialization() {
  int *n = new int;

  // Should warn that *n is uninitialized.
  if (*n) { // no-warning
  }
}

void testValueInitialization() {
  int *n = new int(3);

  // Should be TRUE (and have no uninitialized variable warning)
  clang_analyzer_eval(*n == 3); // expected-warning{{UNKNOWN}}
}


void *operator new(size_t, void *, void *);
void *testCustomNewMalloc() {
  int *x = (int *)malloc(sizeof(int));  

  // Should be no-warning (the custom allocator could have freed x).
  void *y = new (0, x) int; // expected-warning{{leak of memory pointed to by 'x'}}

  return y;
}

