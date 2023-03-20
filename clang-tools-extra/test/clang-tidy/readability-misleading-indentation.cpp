// RUN: %check_clang_tidy %s readability-misleading-indentation %t

void foo1();
void foo2();

#define BLOCK \
  if (cond1)  \
    foo1();   \
    foo2();

void f()
{
  bool cond1 = true;
  bool cond2 = true;

  if (cond1)
    if (cond2)
      foo1();
  else
    foo2();
  // CHECK-MESSAGES: :[[@LINE-2]]:3: warning: different indentation for 'if' and corresponding 'else' [readability-misleading-indentation]

  if (cond1) {
    if (cond2)
      foo1();
  }
  else
    foo2();

  if (cond1)
    if (cond2)
      foo1();
    else
      foo2();

  if (cond2)
    foo1();
    foo2();
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: misleading indentation: statement is indented too deeply [readability-misleading-indentation]
    // CHECK-MESSAGES: :[[@LINE-4]]:3: note: did you mean this line to be inside this 'if'
    foo2(); // No redundant warning.

  if (cond1)
  {
    foo1();
  }
    foo2();

  if (cond1)
    foo1();
  foo2();

  if (cond2)
    if (cond1) foo1(); else foo2();

  if (cond1) {
  } else {
  }

  if (cond1) {
  }
  else {
  }

  if (cond1)
  {
  }
  else
  {
  }

  if (cond1)
    {
    }
  else
    {
    }

  if(cond1) {
  }
  else if (cond2) {
  }
  else {
  }

  if(cond1) {
  }
  else if (cond2) {
  }
       else {
  }
  // CHECK-MESSAGES: :[[@LINE-2]]:8: warning: different indentation for 'if' and corresponding 'else' [readability-misleading-indentation]

  if (cond1) {
    if (cond1) {
    }
    else if (cond2) {
    }
    else {
    }
    if (cond1) {
    } else if (cond2) {
    } else if (!cond2) {
    } else {
    }
  }
  else if (cond2) {
  }

  BLOCK
}

void g(bool x) {
  if (x)
    #pragma unroll
    for (int k = 0; k < 1; ++k) {}

  #pragma unroll
  for (int k = 0; k < 1; ++k) {}
}
