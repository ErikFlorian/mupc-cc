#define ENABLE_MESSAGE 1
#define TWICE(value) ((value) + (value))

#ifdef ENABLE_MESSAGE
int main(void) {
    uint x = TWICE(32);
    _putchar(x);
    _putchar('\n');
    return 0;
}
#else
int main(void) { return 0; }
#endif
