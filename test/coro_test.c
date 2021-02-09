#include <stdio.h>
#include <aekv/coroutine.h>

struct args {
  int n;
};

static void foo(coroutine_t *co, void *ud) {
  struct args *arg = ud;
  int start = arg->n;
  int i;
  for (i = 0; i < 5; i++) {
    printf("coroutine %s : %d\n", coroutine_name(co), start + i);
    coroutine_yield(co);
  }
}

static void test(schedule_t *S) {
  struct args arg1 = {100};
  struct args arg2 = {200};
  struct args arg3 = {300};

  coroutine_t *co1 = coroutine_new(S, foo, &arg1, "one");
  coroutine_t *co2 = coroutine_new(S, foo, &arg2, "two");
  coroutine_t *co3 = coroutine_new(S, foo, &arg3, "thr");
  printf("main start\n");
  while (coroutine_status(co1) && coroutine_status(co2)) {
    coroutine_resume(co1);
    coroutine_resume(co2);
  }
  while (coroutine_status(co3))
    coroutine_resume(co3);
  printf("main end\n");
}

int main() {
  schedule_t *S = coroutine_open();
  test(S);
  coroutine_close(S);

  return 0;
}
