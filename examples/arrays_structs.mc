struct Point {
    uint x;
    uint y;
};

uint sum(uint *values, uint length) {
    uint result = 0;
    for (uint i = 0; i < length; i++) {
        result += *(values + i);
    }
    return result;
}

int main(void) {
    uint values[3] = { 10, 20, 30 };
    struct Point origin = { 65, 66 };
    struct Point *p = &origin;

    _putchar(p->x);
    _putchar(p->y);
    _putchar(sum(values, 3) / 10 + '0');
    _putchar('\n');
    return 0;
}
