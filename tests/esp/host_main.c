#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
uint32_t compute(uint32_t);
int main(int argc, char **argv) { printf("a2=%08x\n", compute(argc > 1 ? (uint32_t)atoi(argv[1]) : 99)); return 0; }
