/* The Online Tourney table's bracket (runtime/hle_tourney_table.c) on views read from stdin, one case
 * a line: "<player>\t<can_end 0/1>\t<the view's JSON on one line>". Prints each case's lines, or "none"
 * when there is no table, then "--". Plain C, no runtime:
 *
 *   clang tools/dev/tourneytabletest.c runtime/hle_tourney_table.c -o tourneytabletest
 *
 * tests/test_bracket.py builds it and checks it against tools/bracket_tui.py. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/tourney_table.h"

int main(void) {
    static char line[1 << 20], out[16384];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) { printf("bad case\n--\n"); continue; }
        *t1 = *t2 = 0;
        if (tourney_table(t2 + 1, line, atoi(t1 + 1), out, sizeof out)) fputs(out, stdout);
        else printf("none\n");
        printf("--\n");
    }
    return 0;
}
