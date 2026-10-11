_origin 0x0100;

int main(void) {
    // Immediate values and I/O channels are emitted as following data bytes.
    _asm("ldi a\ndb 79\nout a\ndb 0\nldi a\ndb 75\nout a\ndb 0\nldi a\ndb 10\nout a\ndb 0");
    return 0;
}
