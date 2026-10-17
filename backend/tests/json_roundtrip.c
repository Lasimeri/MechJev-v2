/* json_roundtrip: each input line parsed, then written compact and pretty,
 * for a byte comparison with serde_json's output on the same lines. */
#include <stdio.h>
#include <string.h>
#include "../src/json.h"

int main(void)
{
    char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n')
            line[--n] = 0;
        json_t *v = json_parse(line, n, NULL, 0);
        if (!v) {
            puts("ERR");
            continue;
        }
        sb_t b;
        sb_init(&b);
        json_write(&b, v);
        puts(b.s);
        sb_free(&b);
        json_write_pretty(&b, v);
        puts(b.s);
        sb_free(&b);
        json_free(v);
    }
    return 0;
}
