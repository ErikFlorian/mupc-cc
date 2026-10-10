uint factorial(uint n);

uint factorial(uint n) {
    if (n < 2) return 1;
    return n * factorial(n - 1);
}

int main(void) {
    // The emulator displays only explicit output, not a function's return value.
    uint answer = factorial(3);
    putchar(answer + '0');
    putchar('\n');
    return 0;
}
