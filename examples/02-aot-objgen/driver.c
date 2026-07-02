/* driver.c — a tiny C main that calls the AOT-compiled function from aot_out.o.
 * The symbol square_plus_one was emitted by aot.cpp with external linkage,
 * so the linker resolves this declaration against it (chapter 03.04). */
#include <stdio.h>

int square_plus_one(int x); /* defined in aot_out.o */

int main(void) {
  for (int i = 0; i < 5; i++)
    printf("square_plus_one(%d) = %d\n", i, square_plus_one(i));
  return 0;
}
