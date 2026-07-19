#ifndef MOER_SHARED_UTILS_MATH_HLSLI
#define MOER_SHARED_UTILS_MATH_HLSLI

namespace Math {
// "Explodes" an integer, i.e. inserts a 0 between each bit.  Takes inputs up to
// 16 bit wide.
//      For example, 0b11111111 -> 0b1010101010101010
uint IntegerExplode(uint x) {
  x = (x | (x << 8)) & 0x00FF00FF;
  x = (x | (x << 4)) & 0x0F0F0F0F;
  x = (x | (x << 2)) & 0x33333333;
  x = (x | (x << 1)) & 0x55555555;
  return x;
}

// Reverse of IntegerExplode, i.e. takes every other bit in the integer
// and compresses those bits into a dense bit firld. Takes 32-bit inputs,
// produces 16-bit outputs.
//    For example, 0b'abcdefgh' -> 0b'0000bdfh'
uint IntegerCompact(uint x) {
  x = (x & 0x11111111) | ((x & 0x44444444) >> 1);
  x = (x & 0x03030303) | ((x & 0x30303030) >> 2);
  x = (x & 0x000F000F) | ((x & 0x0F000F00) >> 4);
  x = (x & 0x000000FF) | ((x & 0x00FF0000) >> 8);
  return x;
}

// Converts a 2D position to a linear index following a Z-curve pattern.
uint ZCurveToLinearIndex(uint2 xy) {
  return IntegerExplode(xy[0]) | (IntegerExplode(xy[1]) << 1);
}

// Converts a linear to a 2D position following a Z-curve pattern.
uint2 LinearIndexToZCurve(uint index) {
  return uint2(IntegerCompact(index), IntegerCompact(index >> 1));
}

// 32 bit Jenkins hash
uint JenkinsHash(uint a) {
  // http://burtleburtle.net/bob/hash/integer.html
  a = (a + 0x7ed55d16) + (a << 12);
  a = (a ^ 0xc761c23c) ^ (a >> 19);
  a = (a + 0x165667b1) + (a << 5);
  a = (a + 0xd3a2646c) ^ (a << 9);
  a = (a + 0xfd7046c5) + (a << 3);
  a = (a ^ 0xb55a4f09) ^ (a >> 16);
  return a;
}

} // namespace Math

#endif
