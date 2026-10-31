/* Sparse destructive RAM test with restoration. Caller must guard ownership,
 * mapping/cache state and reservations before invoking. No physical I/O here.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define RP_MAX_SAMPLES 65536u
#define RP_MAX_BYTES UINT64_C(0x200000000)
struct rp_io {
    void *context;
    uint64_t (*read)(void *, uint64_t);
    void (*write)(void *, uint64_t, uint64_t);
    void (*barrier)(void *);
};
struct rp_result { unsigned phase; uint64_t address, expected, actual; int restored; };

static uint64_t rp_address(uint64_t base, uint64_t bytes, unsigned count, unsigned i)
{ return base + (((bytes - 8) * i / (count - 1)) & ~UINT64_C(7)); }
static uint64_t rp_pattern(uint64_t address)
{
    uint64_t v = address ^ UINT64_C(0x6e624eb7a129d583);
    v = (v ^ (v >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    v = (v ^ (v >> 27)) * UINT64_C(0x94d049bb133111eb);
    return v ^ (v >> 31);
}
/* Return 0 only if both patterns and restored originals verify. Results apply
 * to the sampled words, not every byte. Saved storage must be outside window.
 */
static int rp_run(const volatile struct rp_io *io, uint64_t base, uint64_t bytes,
                  unsigned count, uint64_t *saved, struct rp_result *result)
{
    *result = (struct rp_result){0, 0, 0, 0, 0};
    if (!io || !io->read || !io->write || !io->barrier || !saved ||
        (base & 7) || count < 2 || count > RP_MAX_SAMPLES ||
        bytes < (uint64_t)count * 8 || bytes > RP_MAX_BYTES || bytes > UINT64_MAX - base)
        return 1;
    for (unsigned i = 0; i < count; i++)
        saved[i] = io->read(io->context, rp_address(base, bytes, count, i));
    int failure = 0;
    for (unsigned phase = 1; phase <= 2 && !failure; phase++) {
        for (unsigned i = 0; i < count; i++) {
            uint64_t a = rp_address(base, bytes, count, i), v = rp_pattern(a);
            io->write(io->context, a, phase == 1 ? v : ~v);
        }
        io->barrier(io->context);
        for (unsigned i = 0; i < count; i++) {
            uint64_t a = rp_address(base, bytes, count, i), v = rp_pattern(a);
            uint64_t expected = phase == 1 ? v : ~v, actual = io->read(io->context, a);
            if (actual != expected) {
                *result = (struct rp_result){phase, a, expected, actual, 0};
                failure = 2;
                break;
            }
        }
    }
    for (unsigned i = count; i > 0; i--)
        io->write(io->context, rp_address(base, bytes, count, i - 1), saved[i - 1]);
    io->barrier(io->context);
    result->restored = 1;
    for (unsigned i = 0; i < count; i++) {
        uint64_t a = rp_address(base, bytes, count, i), actual = io->read(io->context, a);
        if (actual != saved[i]) {
            if (!failure) *result = (struct rp_result){3, a, saved[i], actual, 0};
            result->restored = 0;
            failure = 3;
            break;
        }
    }
    return failure;
}
