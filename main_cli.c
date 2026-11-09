#include <stdio.h>
#include <stdlib.h>
int pt_all(char *buf, unsigned long cap, int states, int touch);
int main(int argc, char **argv) {
    static char buf[1 << 20];
    int bad = pt_all(buf, sizeof buf, argc > 1 ? atoi(argv[1]) : 256, 0);
    fputs(buf, stdout);
    return bad ? 1 : 0;
}
