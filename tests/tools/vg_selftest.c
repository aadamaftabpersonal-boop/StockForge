/* Deliberately broken program: proves scripts/valgrind-sf.sh really fails on memory errors.
 * Contains (1) a branch on an uninitialised value and (2) a definitely-lost allocation. */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char *p = malloc(8);
    if (p[3] == 42) puts("unlikely");     /* uninitialised read */
    p = NULL;                              /* leak: the block is now unreachable */
    return 0;
}
