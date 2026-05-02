#include "gtest/gtest.h"
#include <cstring>
#include <cstdint>
#include <vector>

// ── Inline the core BPC logic as free functions ──────────────────────────────
// This avoids needing the SimObject/Params machinery entirely.

static constexpr int WORDS = 16;
static constexpr int BITS  = 32;
// A plane only has WORDS valid bits (extractPlane sets bits 0..WORDS-1).
static constexpr uint32_t PLANE_MASK =
    (WORDS == 32) ? 0xFFFFFFFFu : ((1u << WORDS) - 1u);

static uint32_t extractPlane(const uint32_t* words, int bit) {
    uint32_t plane = 0;
    for (int w = 0; w < WORDS; w++)
        if ((words[w] >> bit) & 1u)
            plane |= (1u << w);
    return plane;
}

struct BPCResult {
    uint8_t  tags[BITS];
    uint32_t raw[BITS];
    int      size_bits;
};

static BPCResult compress(const uint8_t* line) {
    const uint32_t* words = reinterpret_cast<const uint32_t*>(line);
    BPCResult r{};
    uint32_t prev = 0;
    int raw_idx = 0;

    for (int i = 0; i < BITS; i++) {
        uint32_t plane = extractPlane(words, i);
        if      (plane == 0u)                       { r.tags[i] = 0; r.size_bits += 3; }
        else if (plane == PLANE_MASK)               { r.tags[i] = 1; r.size_bits += 3; }
        else if (plane == prev)                     { r.tags[i] = 2; r.size_bits += 3; }
        else if (plane == ((~prev) & PLANE_MASK))   { r.tags[i] = 3; r.size_bits += 3; }
        else { r.tags[i] = 4; r.raw[raw_idx++] = plane; r.size_bits += 3 + 16; }
        prev = plane;
    }
    return r;
}

static void decompress(const BPCResult& r, uint8_t* out) {
    uint32_t planes[BITS];
    uint32_t prev = 0;
    int raw_idx = 0;

    for (int i = 0; i < BITS; i++) {
        switch (r.tags[i]) {
            case 0: planes[i] = 0u;                    break;
            case 1: planes[i] = PLANE_MASK;            break;
            case 2: planes[i] = prev;                  break;
            case 3: planes[i] = (~prev) & PLANE_MASK;  break;
            case 4: planes[i] = r.raw[raw_idx++];      break;
        }
        prev = planes[i];
    }

    uint32_t words[WORDS] = {};
    for (int bit = 0; bit < BITS; bit++)
        for (int w = 0; w < WORDS; w++)
            if ((planes[bit] >> w) & 1u)
                words[w] |= (1u << bit);

    memcpy(out, words, 64);
}

// ── Tests ────────────────────────────────────────────────────────────────────

// All-zero line: every plane is ALL_ZEROS → 32 × 3 = 96 bits
TEST(BPCTest, AllZerosSize) {
    uint8_t line[64] = {};
    auto r = compress(line);
    EXPECT_EQ(r.size_bits, 96);
}

// All-zero line: max compression, well below uncompressed 512 bits
TEST(BPCTest, AllZerosCompresses) {
    uint8_t line[64] = {};
    auto r = compress(line);
    EXPECT_LT(r.size_bits, 512);
}

// All-ones line: every plane is ALL_ONES → 32 × 3 = 96 bits
TEST(BPCTest, AllOnesSize) {
    uint8_t line[64];
    memset(line, 0xFF, 64);
    auto r = compress(line);
    EXPECT_EQ(r.size_bits, 96);
}

// Alternating 0x00000000 / 0xFFFFFFFF words:
// plane 0: alternating bits → UNCOMPRESSED
// plane 1: same as plane 0 → SAME_AS_PREV ... etc.
// Just verify it round-trips correctly
TEST(BPCTest, AlternatingWordsRoundTrip) {
    uint32_t words[16];
    for (int i = 0; i < 16; i++)
        words[i] = (i % 2 == 0) ? 0x00000000u : 0xFFFFFFFFu;

    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}

// Round-trip: sequential bytes 0..63
TEST(BPCTest, SequentialRoundTrip) {
    uint8_t line[64], out[64] = {};
    for (int i = 0; i < 64; i++) line[i] = (uint8_t)i;

    auto r = compress(line);
    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}

// Round-trip: all same word repeated (pointer-heavy workload)
TEST(BPCTest, RepeatedWordRoundTrip) {
    uint32_t words[16];
    for (int i = 0; i < 16; i++) words[i] = 0xDEADBEEFu;
    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}

// Round-trip: random-ish data (worst case for compression)
TEST(BPCTest, RandomDataRoundTrip) {
    uint8_t line[64], out[64] = {};
    // deterministic "random" pattern
    for (int i = 0; i < 64; i++) line[i] = (uint8_t)(i * 37 + 13);

    auto r = compress(line);
    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}

// Uncompressed size is 512 bits (16 words × 32 bits)
TEST(BPCTest, UncompressedSizeUpperBound) {
    uint8_t line[64], out[64] = {};
    for (int i = 0; i < 64; i++) line[i] = (uint8_t)(i * 37 + 13);

    auto r = compress(line);
    // worst case: all 32 planes uncompressed = 32 × (3+16) = 608 bits
    // but never more than that
    EXPECT_LE(r.size_bits, 32 * (3 + 16));
}

// Single non-zero word: only plane 0 differs, rest are zeros
TEST(BPCTest, SingleNonZeroWordCompresses) {
    uint8_t line[64] = {};
    // set words[0] = 1
    line[0] = 1;

    auto r = compress(line);
    // should compress significantly vs 512 bits
    EXPECT_LT(r.size_bits, 200);
}

// Plane 1 is the bitwise complement of plane 0 (within PLANE_MASK).
// Forces COMP_OF_PREV — the path that was unreachable before the
// PLANE_MASK fix, so this test would have caught that bug.
TEST(BPCTest, ComplementOfPrevRoundTrip) {
    uint32_t words[16] = {};
    for (int w = 0; w < 16; w++) {
        // bit 0 set on even words, bit 1 set on odd words.
        // → plane 0 = 0x5555, plane 1 = 0xAAAA = ~0x5555 & PLANE_MASK
        if (w % 2 == 0) words[w] |= (1u << 0);
        else            words[w] |= (1u << 1);
    }
    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);

    bool saw_comp_of_prev = false;
    for (int i = 0; i < BITS; i++)
        if (r.tags[i] == 3) { saw_comp_of_prev = true; break; }
    EXPECT_TRUE(saw_comp_of_prev);
}

// Every plane after the first is the complement of the previous plane.
// Direct size regression: 19 (UNCOMPRESSED plane 0) + 31×3 (COMP_OF_PREV).
TEST(BPCTest, ComplementOfPrevSize) {
    // word w = 0x55555555 if bit w of plane0 is set, else 0xAAAAAAAA.
    // Choose plane0 = 0x5555 so bits 0,2,4,...,14 of plane = 1.
    uint32_t words[16];
    for (int w = 0; w < 16; w++)
        words[w] = (w % 2 == 0) ? 0x55555555u : 0xAAAAAAAAu;

    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    EXPECT_EQ(r.size_bits, 19 + 31 * 3);

    int comp_count = 0;
    for (int i = 0; i < BITS; i++) if (r.tags[i] == 3) comp_count++;
    EXPECT_EQ(comp_count, 31);

    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}

// Construct a line that emits every one of the 5 plane encodings.
// Per-word bit layout:
//   bit 0 = 0           → plane 0  = 0x0000  (ALL_ZEROS)
//   bit 1 = 1           → plane 1  = 0xFFFF  (ALL_ONES)
//   bit 2 = (w even)    → plane 2  = 0x5555  (UNCOMPRESSED, prev=0xFFFF)
//   bit 3 = (w odd)     → plane 3  = 0xAAAA  (COMP_OF_PREV of plane 2)
//   bit 4 = (w odd)     → plane 4  = 0xAAAA  (SAME_AS_PREV)
//   bits 5..31 = 0      → planes 5..31 = 0   (ALL_ZEROS)
TEST(BPCTest, MixedTagsRoundTrip) {
    uint32_t words[16] = {};
    for (int w = 0; w < 16; w++) {
        words[w] |= (1u << 1);
        if (w % 2 == 0) words[w] |= (1u << 2);
        else            words[w] |= (1u << 3) | (1u << 4);
    }
    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);

    bool seen[5] = {false, false, false, false, false};
    for (int i = 0; i < BITS; i++) seen[r.tags[i]] = true;
    for (int t = 0; t < 5; t++) EXPECT_TRUE(seen[t]) << "tag " << t;
}

// Exactly one plane is all-set (PLANE_MASK), the rest are zero.
// Exercises ALL_ONES emission outside the uniform-line scenario.
TEST(BPCTest, AllOnesIsolatedPlane) {
    uint32_t words[16];
    // bit 5 set in every word, all other bits zero
    for (int w = 0; w < 16; w++) words[w] = (1u << 5);

    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    EXPECT_EQ(r.tags[5], 1);
    for (int i = 0; i < BITS; i++)
        if (i != 5) EXPECT_EQ(r.tags[i], 0) << "plane " << i;

    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}

// A single bit set in a single word produces a sparse non-uniform plane
// (neither 0 nor PLANE_MASK), exercising UNCOMPRESSED storage of an
// intermediate-density plane.
TEST(BPCTest, SparsePlaneUncompressed) {
    uint32_t words[16] = {};
    words[7] = (1u << 2);  // → plane 2 has only bit 7 set = 0x0080

    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    EXPECT_EQ(r.tags[2], 4);
    EXPECT_EQ(r.raw[0], 0x0080u);

    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}

// Plane 0 = 0xFF00 — looks "all-ones-ish" in the high byte but isn't
// PLANE_MASK. This is the exact ambiguity the original bug straddled
// (where ~0u was used instead of PLANE_MASK).
TEST(BPCTest, EmptyVsMaskBoundary) {
    uint32_t words[16];
    for (int w = 0; w < 16; w++) words[w] = (w < 8) ? 0u : 0xFFFFFFFFu;

    uint8_t line[64], out[64] = {};
    memcpy(line, words, 64);

    auto r = compress(line);
    EXPECT_EQ(r.tags[0], 4);             // UNCOMPRESSED, plane = 0xFF00
    EXPECT_EQ(r.raw[0], 0xFF00u);
    for (int i = 1; i < BITS; i++)
        EXPECT_EQ(r.tags[i], 2) << "plane " << i;  // SAME_AS_PREV
    EXPECT_EQ(r.size_bits, 19 + 31 * 3);

    decompress(r, out);
    EXPECT_EQ(memcmp(line, out, 64), 0);
}
