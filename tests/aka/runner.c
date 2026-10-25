#include "../../aka.c"
void aka_audio_sample(int16_t s, uint64_t c) {
  (void)s;
  (void)c;
}
int aka_sd_read(uint32_t a, uint8_t *b) {
  (void)a;
  (void)b;
  return 0;
}
int aka_sd_write(uint32_t a, const uint8_t *b) {
  (void)a;
  (void)b;
  return 0;
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  FILE *f = fopen(argv[1], "rb");
  if (!f)
    return 2;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  uint8_t *b = malloc(n);
  if (fread(b, 1, n, f) != (size_t)n)
    return 2;
  fclose(f);
  int r = aka_load(b, n, NULL, 0);
  free(b);
  if (r) {
    fprintf(stderr, "load %d\n", r);
    return 2;
  }
  aka_run(10000000);
  printf("%08x %s\n", xt_load(symbol("result"), 4),
         aka_error() ? aka_error() : "running");
  return xt.halted == 2 ? 0 : 1;
}
