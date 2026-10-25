#include <stdint.h>
volatile uint32_t result;
__attribute__((noinline)) uint32_t recurse(unsigned n, uint32_t v) {
  uint32_t a = v * 17 + 3, b = v ^ 0x987abc;
  if (n)
    v = recurse(n - 1, v + 7);
  return (v + a) ^ b;
}
__attribute__((noinline)) uint32_t floats(float x, float y) {
  for (unsigned i = 0; i < 30; i++) {
    x = (x + y) * 0.75f;
    if (x > 50.0f)
      y = -y;
    if (x < -50.0f)
      y = -y;
    y += 0.125f;
  }
  return (uint32_t)(int32_t)x ^ ((uint32_t)(int32_t)y << 16);
}
uint32_t compute(void) { return recurse(90, 0x12345678) ^ floats(3.5f, 7.75f); }
void m0boy_console_main(void) { result = compute(); }
