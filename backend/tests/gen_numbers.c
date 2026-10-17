/* gen_numbers: lines of JSON arrays of random numbers in many spellings
 * (short decimals, long ones past 17 digits, exponents both ways, huge
 * integers), for the serde_json comparison. Seeded, so repeatable. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int lines = argc > 1 ? atoi(argv[1]) : 1000;
    srand(20261017);
    for (int l = 0; l < lines; l++) {
        putchar('[');
        for (int k = 0; k < 20; k++) {
            if (k)
                putchar(',');
            if (rand() % 2)
                putchar('-');
            int kind = rand() % 6;
            int nd = 1 + rand() % 22;
            putchar('1' + rand() % 9);
            for (int i = 1; i < nd; i++)
                putchar('0' + rand() % 10);
            if (kind >= 2) {
                putchar('.');
                int nf = 1 + rand() % 20;
                for (int i = 0; i < nf; i++)
                    putchar('0' + rand() % 10);
            }
            if (kind >= 4)
                printf("e%c%d", rand() % 2 ? '+' : '-', rand() % 320);
        }
        puts("]");
    }
    return 0;
}
