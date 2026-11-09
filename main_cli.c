#include <stdio.h>
#include <stdlib.h>
#ifndef PT_ENTRY
#define PT_ENTRY pt_all
#endif
int PT_ENTRY(char *buf, unsigned long cap, int states, int touch);
int main(int argc, char **argv) {
    static char buf[1 << 20];
    int bad = PT_ENTRY(buf, sizeof buf, argc > 1 ? atoi(argv[1]) : 256, 0);
    fputs(buf, stdout);
    return bad ? 1 : 0;
}
