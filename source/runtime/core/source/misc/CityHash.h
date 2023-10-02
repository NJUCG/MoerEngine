
// Copyright (c) 2011 Google, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// CityHash Version 1, by Geoff Pike and Jyrki Alakuijala
//
// This file provides a few functions for hashing strings. On x86-64
// hardware in 2011, CityHash64() is faster than other high-quality
// hash functions, such as Murmur.  This is largely due to higher
// instruction-level parallelism.  CityHash64() and CityHash128() also perform
// well on hash-quality tests.
//
// CityHash128() is optimized for relatively long strings and returns
// a 128-bit hash.  For strings more than about 2000 bytes it can be
// faster than CityHash64().
//
// Functions in the CityHash family are not suitable for cryptography.
//
// WARNING: This code has not been tested on big-endian platforms!
// It is known to work well on little-endian platforms that have a small penalty
// for unaligned reads, such as current Intel and AMD moderate-to-high-end CPUs.
//
// By the way, for some hash functions, given strings a and b, the hash
// of a+b is easily derived from the hashes of a and b.  This property
// doesn't hold for any hash functions in this file.

#ifndef CITY_HASH_H_
#define CITY_HASH_H_

#if defined(_MSC_VER) || defined(__CYGWIN__)
#include "pstdint.h"

#pragma warning(disable:4267)
#else
#include <stdint.h>
#endif

#include <stdlib.h>  // for size_t.
#include <utility>

typedef std::pair<uint64_t, uint64_t> uint128_t;

inline uint64_t Uint128Low64(const uint128_t& x) { return x.first; }
inline uint64_t Uint128High64(const uint128_t& x) { return x.second; }

// Hash function for a byte array.
uint64_t CityHash64(const char *buf, size_t len);

// Hash function for a byte array.  For convenience, a 64-bit seed is also
// hashed into the result.
uint64_t CityHash64WithSeed(const char *buf, size_t len, uint64_t seed);

// Hash function for a byte array.  For convenience, two seeds are also
// hashed into the result.
uint64_t CityHash64WithSeeds(const char *buf, size_t len,
                           uint64_t seed0, uint64_t seed1);

// Hash function for a byte array.
uint128_t CityHash128(const char *s, size_t len);

// Hash function for a byte array.  For convenience, a 128-bit seed is also
// hashed into the result.
uint128_t CityHash128WithSeed(const char *s, size_t len, uint128_t seed);

// Hash 128 input bits down to 64 bits of output.
// This is intended to be a reasonably good hash function.
inline uint64_t Hash128to64(const uint128_t& x) {
    // Murmur-inspired hashing.
    const uint64_t kMul = 0x9ddfea08eb382d69ULL;
    uint64_t a = (Uint128Low64(x) ^ Uint128High64(x)) * kMul;
    a ^= (a >> 47);
    uint64_t b = (Uint128High64(x) ^ a) * kMul;
    b ^= (b >> 47);
    b *= kMul;
    return b;
}


#include <algorithm>

#define UNALIGNED_LOAD64(p) (*(const uint64_t*)(p))
#define UNALIGNED_LOAD32(p) (*(const uint32_t*)(p))

#if !defined(LIKELY)
#if defined(__GNUC__)
#define LIKELY(x) (__builtin_expect(!!(x), 1))
#else
#define LIKELY(x) (x)
#endif
#endif

// Some primes between 2^63 and 2^64 for various uses.
static const uint64_t k0 = 0xc3a5c85c97cb3127ULL;
static const uint64_t k1 = 0xb492b66fbe98f273ULL;
static const uint64_t k2 = 0x9ae16a3b2f90404fULL;
static const uint64_t k3 = 0xc949d7c7509e6557ULL;

// Bitwise right rotate.  Normally this will compile to a single
// instruction, especially if the shift is a manifest constant.
static uint64_t Rotate(uint64_t val, int shift) {
    // Avoid shifting by 64: doing so yields an undefined result.
    return shift == 0 ? val : ((val >> shift) | (val << (64 - shift)));
}

// Equivalent to Rotate(), but requires the second arg to be non-zero.
// On x86-64, and probably others, it's possible for this to compile
// to a single instruction if both args are already in registers.
static uint64_t RotateByAtLeast1(uint64_t val, int shift) {
    return (val >> shift) | (val << (64 - shift));
}

static uint64_t ShiftMix(uint64_t val) {
    return val ^ (val >> 47);
}

static uint64_t HashLen16(uint64_t u, uint64_t v) {
    return Hash128to64(uint128_t(u, v));
}

static uint64_t HashLen0to16(const char *s, size_t len) {
    if (len > 8) {
        uint64_t a = UNALIGNED_LOAD64(s);
        uint64_t b = UNALIGNED_LOAD64(s + len - 8);
        return HashLen16(a, RotateByAtLeast1(b + len, len)) ^ b;
    }
    if (len >= 4) {
        uint64_t a = UNALIGNED_LOAD32(s);
        return HashLen16(len + (a << 3), UNALIGNED_LOAD32(s + len - 4));
    }
    if (len > 0) {
        uint8_t a = s[0];
        uint8_t b = s[len >> 1];
        uint8_t c = s[len - 1];
        uint32_t y = static_cast<uint32_t>(a) + (static_cast<uint32_t>(b) << 8);
        uint32_t z = len + (static_cast<uint32_t>(c) << 2);
        return ShiftMix(y * k2 ^ z * k3) * k2;
    }
    return k2;
}

// This probably works well for 16-byte strings as well, but it may be overkill
// in that case.
static uint64_t HashLen17to32(const char *s, size_t len) {
    uint64_t a = UNALIGNED_LOAD64(s) * k1;
    uint64_t b = UNALIGNED_LOAD64(s + 8);
    uint64_t c = UNALIGNED_LOAD64(s + len - 8) * k2;
    uint64_t d = UNALIGNED_LOAD64(s + len - 16) * k0;
    return HashLen16(Rotate(a - b, 43) + Rotate(c, 30) + d,
                     a + Rotate(b ^ k3, 20) - c + len);
}

// Return a 16-byte hash for 48 bytes.  Quick and dirty.
// Callers do best to use "random-looking" values for a and b.
static std::pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(
    uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b) {
    a += w;
    b = Rotate(b + a + z, 21);
    uint64_t c = a;
    a += x;
    a += y;
    b += Rotate(a, 44);
    return std::make_pair(a + z, b + c);
}

// Return a 16-byte hash for s[0] ... s[31], a, and b.  Quick and dirty.
static std::pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(
    const char* s, uint64_t a, uint64_t b) {
    return WeakHashLen32WithSeeds(UNALIGNED_LOAD64(s),
                                  UNALIGNED_LOAD64(s + 8),
                                  UNALIGNED_LOAD64(s + 16),
                                  UNALIGNED_LOAD64(s + 24),
                                  a,
                                  b);
}

// Return an 8-byte hash for 33 to 64 bytes.
static uint64_t HashLen33to64(const char *s, size_t len) {
    uint64_t z = UNALIGNED_LOAD64(s + 24);
    uint64_t a = UNALIGNED_LOAD64(s) + (len + UNALIGNED_LOAD64(s + len - 16)) * k0;
    uint64_t b = Rotate(a + z, 52);
    uint64_t c = Rotate(a, 37);
    a += UNALIGNED_LOAD64(s + 8);
    c += Rotate(a, 7);
    a += UNALIGNED_LOAD64(s + 16);
    uint64_t vf = a + z;
    uint64_t vs = b + Rotate(a, 31) + c;
    a = UNALIGNED_LOAD64(s + 16) + UNALIGNED_LOAD64(s + len - 32);
    z = UNALIGNED_LOAD64(s + len - 8);
    b = Rotate(a + z, 52);
    c = Rotate(a, 37);
    a += UNALIGNED_LOAD64(s + len - 24);
    c += Rotate(a, 7);
    a += UNALIGNED_LOAD64(s + len - 16);
    uint64_t wf = a + z;
    uint64_t ws = b + Rotate(a, 31) + c;
    uint64_t r = ShiftMix((vf + ws) * k2 + (wf + vs) * k0);
    return ShiftMix(r * k0 + vs) * k2;
}

uint64_t CityHash64(const char *s, size_t len) {
    if (len <= 32) {
        if (len <= 16) {
            return HashLen0to16(s, len);
        } else {
            return HashLen17to32(s, len);
        }
    } else if (len <= 64) {
        return HashLen33to64(s, len);
    }

    // For strings over 64 bytes we hash the end first, and then as we
    // loop we keep 56 bytes of state: v, w, x, y, and z.
    uint64_t x = UNALIGNED_LOAD64(s);
    uint64_t y = UNALIGNED_LOAD64(s + len - 16) ^ k1;
    uint64_t z = UNALIGNED_LOAD64(s + len - 56) ^ k0;
    std::pair<uint64_t, uint64_t> v = WeakHashLen32WithSeeds(s + len - 64, len, y);
    std::pair<uint64_t, uint64_t> w = WeakHashLen32WithSeeds(s + len - 32, len * k1, k0);
    z += ShiftMix(v.second) * k1;
    x = Rotate(z + x, 39) * k1;
    y = Rotate(y, 33) * k1;

    // Decrease len to the nearest multiple of 64, and operate on 64-byte chunks.
    len = (len - 1) & ~static_cast<size_t>(63);
    do {
        x = Rotate(x + y + v.first + UNALIGNED_LOAD64(s + 16), 37) * k1;
        y = Rotate(y + v.second + UNALIGNED_LOAD64(s + 48), 42) * k1;
        x ^= w.second;
        y ^= v.first;
        z = Rotate(z ^ w.first, 33);
        v = WeakHashLen32WithSeeds(s, v.second * k1, x + w.first);
        w = WeakHashLen32WithSeeds(s + 32, z + w.second, y);
        std::swap(z, x);
        s += 64;
        len -= 64;
    } while (len != 0);
    return HashLen16(HashLen16(v.first, w.first) + ShiftMix(y) * k1 + z,
                     HashLen16(v.second, w.second) + x);
}

uint64_t CityHash64WithSeed(const char *s, size_t len, uint64_t seed) {
    return CityHash64WithSeeds(s, len, k2, seed);
}

uint64_t CityHash64WithSeeds(const char *s, size_t len,
                           uint64_t seed0, uint64_t seed1) {
    return HashLen16(CityHash64(s, len) - seed0, seed1);
}

// A subroutine for CityHash128().  Returns a decent 128-bit hash for strings
// of any length representable in ssize_t.  Based on City and Murmur.
static uint128_t CityMurmur(const char *s, size_t len, uint128_t seed) {
    uint64_t a = Uint128Low64(seed);
    uint64_t b = Uint128High64(seed);
    uint64_t c = 0;
    uint64_t d = 0;
    ssize_t l = len - 16;
    if (l <= 0) {  // len <= 16
        c = b * k1 + HashLen0to16(s, len);
        d = Rotate(a + (len >= 8 ? UNALIGNED_LOAD64(s) : c), 32);
    } else {  // len > 16
        c = HashLen16(UNALIGNED_LOAD64(s + len - 8) + k1, a);
        d = HashLen16(b + len, c + UNALIGNED_LOAD64(s + len - 16));
        a += d;
        do {
            a ^= ShiftMix(UNALIGNED_LOAD64(s) * k1) * k1;
            a *= k1;
            b ^= a;
            c ^= ShiftMix(UNALIGNED_LOAD64(s + 8) * k1) * k1;
            c *= k1;
            d ^= c;
            s += 16;
            l -= 16;
        } while (l > 0);
    }
    a = HashLen16(a, c);
    b = HashLen16(d, b);
    return uint128_t(a ^ b, HashLen16(b, a));
}

inline uint128_t CityHash128WithSeed(const char *s, size_t len, uint128_t seed) {
    if (len < 128) {
        return CityMurmur(s, len, seed);
    }

    // We expect len >= 128 to be the common case.  Keep 56 bytes of state:
    // v, w, x, y, and z.
    std::pair<uint64_t, uint64_t> v, w;
    uint64_t x = Uint128Low64(seed);
    uint64_t y = Uint128High64(seed);
    uint64_t z = len * k1;
    v.first = Rotate(y ^ k1, 49) * k1 + UNALIGNED_LOAD64(s);
    v.second = Rotate(v.first, 42) * k1 + UNALIGNED_LOAD64(s + 8);
    w.first = Rotate(y + z, 35) * k1 + x;
    w.second = Rotate(x + UNALIGNED_LOAD64(s + 88), 53) * k1;

    // This is the same inner loop as CityHash64(), manually unrolled.
    do {
        x = Rotate(x + y + v.first + UNALIGNED_LOAD64(s + 16), 37) * k1;
        y = Rotate(y + v.second + UNALIGNED_LOAD64(s + 48), 42) * k1;
        x ^= w.second;
        y ^= v.first;
        z = Rotate(z ^ w.first, 33);
        v = WeakHashLen32WithSeeds(s, v.second * k1, x + w.first);
        w = WeakHashLen32WithSeeds(s + 32, z + w.second, y);
        std::swap(z, x);
        s += 64;
        x = Rotate(x + y + v.first + UNALIGNED_LOAD64(s + 16), 37) * k1;
        y = Rotate(y + v.second + UNALIGNED_LOAD64(s + 48), 42) * k1;
        x ^= w.second;
        y ^= v.first;
        z = Rotate(z ^ w.first, 33);
        v = WeakHashLen32WithSeeds(s, v.second * k1, x + w.first);
        w = WeakHashLen32WithSeeds(s + 32, z + w.second, y);
        std::swap(z, x);
        s += 64;
        len -= 128;
    } while (LIKELY(len >= 128));
    y += Rotate(w.first, 37) * k0 + z;
    x += Rotate(v.first + z, 49) * k0;
    // If 0 < len < 128, hash up to 4 chunks of 32 bytes each from the end of s.
    for (size_t tail_done = 0; tail_done < len; ) {
        tail_done += 32;
        y = Rotate(y - x, 42) * k0 + v.second;
        w.first += UNALIGNED_LOAD64(s + len - tail_done + 16);
        x = Rotate(x, 49) * k0 + w.first;
        w.first += v.first;
        v = WeakHashLen32WithSeeds(s + len - tail_done, v.first, v.second);
    }
    // At this point our 48 bytes of state should contain more than
    // enough information for a strong 128-bit hash.  We use two
    // different 48-byte-to-8-byte hashes to get a 16-byte final result.
    x = HashLen16(x, v.first);
    y = HashLen16(y, w.first);
    return uint128_t(HashLen16(x + v.second, w.second) + y,
                   HashLen16(x + w.second, y + v.second));
}

inline uint128_t CityHash128(const char *s, size_t len) {
    if (len >= 16) {
        return CityHash128WithSeed(s + 16,
                                   len - 16,
                                   uint128_t(UNALIGNED_LOAD64(s) ^ k3,
                                           UNALIGNED_LOAD64(s + 8)));
    } else if (len >= 8) {
        return CityHash128WithSeed(NULL,
                                   0,
                                   uint128_t(UNALIGNED_LOAD64(s) ^ (len * k0),
                                           UNALIGNED_LOAD64(s + len - 8) ^ k1));
    } else {
        return CityHash128WithSeed(s, len, uint128_t(k0, k1));
    }
}

void CityHash64_test ( const void * key, int len, uint32_t seed, void * out )
{
    *(uint64_t *)out = CityHash64WithSeed((const char *)key,len,seed);
}

void CityHash128_test ( const void * key, int len, uint32_t seed, void * out )
{
    uint128_t s(0,0);

    s.first = seed;

    *(uint128_t *)out = CityHash128WithSeed((const char*)key,len,s);
}

#endif  // CITY_HASH_H_
