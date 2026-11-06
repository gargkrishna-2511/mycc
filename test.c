int fib(int n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

int main() {
  int sum = 0;
  for (int i = 1; i <= 10; i = i + 1) {
    if (i % 2 == 0 && i != 6) sum = sum + i;
  }
  printf("sum=%d fib(10)=%d\n", sum, fib(10));
  return 0;
}
