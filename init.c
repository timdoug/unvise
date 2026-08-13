#include "unvise.h"

static void put_word(Buffer out, size_t *op, const uint8_t *word) {
    if (*op >= out.n)
        return;
    out.p[(*op)++] = word[0];
    if (*op < out.n)
        out.p[(*op)++] = word[1];
}

Buffer unpack_code(Buffer p, Buffer code) {
    if (be32(p, 0) != UINT32_C(0xa89f000c))
        die("bad packed-code magic");

    uint32_t out_n = be32(p, 8), ctrl32 = be32(p, 16), flags = be32(p, 20);

    if ((flags & UINT32_C(0xfffffffc)) != UINT32_C(0x80000000))
        die("unsupported packed-code flags");

    /*
     * A89F000C is InstallerVISE's packed-code wrapper, not DEFLATE. Control
     * bytes select literal words, words from a dictionary in CODE 24, or
     * back-references into the output. The dictionary base expression below
     * comes from the installer's 68K unpacker and was checked against both its
     * 68K and PowerPC Dcmp resources.
     */
    size_t ctrl = ctrl32, lit = 24, op = 0;
    size_t dict = 0x4fe + be16(code, 0x4fe + 6 + (flags & 3) * 2);
    Buffer out = {(uint8_t *)calloc(out_n ? out_n : 1, 1), out_n};

    if (!out.p)
        die("out of memory");

#define WORD(B, O)                                                                                 \
    (span((B), (O), 2) ? (B).p + (O) : (die("packed-code word past end"), (uint8_t *)0))
#define EMIT(W) put_word(out, &op, (W))
#define LITERAL()                                                                                  \
    do {                                                                                           \
        EMIT(WORD(p, lit));                                                                        \
        lit += 2;                                                                                  \
    } while (0)

    while (op < out.n) {
        if (!span(p, ctrl, 1))
            die("packed-code control past end");
        unsigned v = p.p[ctrl++];
        if (!(v & 1)) {
            EMIT(WORD(code, dict + (v >> 1) * 2));
            continue;
        }
        v >>= 1;
        if (v & 1) {
            v >>= 1;
            if (!(v & 1)) {
                v >>= 1;
                if (!span(p, ctrl, 1))
                    die("packed-code control past end");
                unsigned index = ((unsigned)p.p[ctrl++] << 5) | v;
                index = (index + 0x80) * 2;
                if (index & 0x2000)
                    LITERAL();
                EMIT(WORD(code, dict + (index & ~0x2000u)));
            } else {
                v >>= 1;
                if (v & 1) {
                    unsigned count = (v >> 1) + 1;
                    while (count--)
                        LITERAL();
                } else {
                    unsigned count = (v >> 1) + 2, raw = be16(p, ctrl);
                    ctrl += 2;
                    if (raw & 0x8000)
                        LITERAL();
                    size_t src = (raw * 2) & 0xffff;
                    while (count--) {
                        EMIT(WORD(out, src));
                        src += 2;
                    }
                }
            }
        } else {
            v >>= 1;
            unsigned count = (v & 7) + 2;
            v >>= 3;
            if (!span(p, ctrl, 1))
                die("packed-code control past end");
            unsigned distance = ((unsigned)p.p[ctrl++] << 3) | v;
            size_t bytes = ((size_t)distance + 1) * 2;
            if (bytes > op)
                die("packed-code back-reference before output");
            size_t src = op - bytes;
            while (count--) {
                EMIT(WORD(out, src));
                src += 2;
            }
        }
    }
#undef LITERAL
#undef EMIT
#undef WORD

    return out;
}

unsigned find_permutation(Buffer b, uint8_t table[256]) {
    unsigned matches = 0;

    if (b.n < 256)
        return 0;

    for (size_t start = 0; start <= b.n - 256; start++) {
        bool seen[256] = {0};
        unsigned i;

        for (i = 0; i < 256; i++) {
            uint8_t value = b.p[start + i];

            if (seen[value])
                break;
            seen[value] = true;
        }

        if (i == 256) {
            memcpy(table, b.p + start, 256);
            matches++;
        }
    }

    return matches;
}

static size_t pef_value(Buffer packed, size_t *position) {
    size_t value = 0;

    for (;;) {
        if (*position >= packed.n || value > (SIZE_MAX >> 7))
            die("invalid PEF packed-data value");

        unsigned byte = packed.p[(*position)++];
        value = (value << 7) | (byte & 0x7f);

        if (!(byte & 0x80))
            return value;
    }
}

static Buffer unpack_pef_data(Buffer packed, size_t expanded_size) {
    Buffer out = {calloc(expanded_size ? expanded_size : 1, 1), expanded_size};
    size_t ip = 0, op = 0;

    if (!out.p)
        die("out of memory");

#define PEF_SPACE(N)                                                                               \
    do {                                                                                           \
        if ((N) > expanded_size - op)                                                              \
            die("PEF packed-data output overflow");                                                \
    } while (0)
#define PEF_INPUT(N)                                                                               \
    do {                                                                                           \
        if ((N) > packed.n - ip)                                                                   \
            die("truncated PEF packed-data section");                                              \
    } while (0)

    while (op < expanded_size) {
        PEF_INPUT(1);
        unsigned control = packed.p[ip++], opcode = control >> 5;
        size_t count = control & 31;

        if (!count)
            count = pef_value(packed, &ip);
        if (!count)
            die("zero-length PEF packed-data operation");

        if (opcode == 0) {
            PEF_SPACE(count);
            op += count;
        } else if (opcode == 1) {
            PEF_INPUT(count);
            PEF_SPACE(count);
            memcpy(out.p + op, packed.p + ip, count);
            ip += count;
            op += count;
        } else if (opcode == 2) {
            size_t repeats = pef_value(packed, &ip) + 1;
            PEF_INPUT(count);
            if (count && repeats > (expanded_size - op) / count)
                die("PEF repeated block overflow");
            for (size_t i = 0; i < repeats; i++) {
                memcpy(out.p + op, packed.p + ip, count);
                op += count;
            }
            ip += count;
        } else if (opcode == 3 || opcode == 4) {
            size_t custom = pef_value(packed, &ip), repeats = pef_value(packed, &ip);
            size_t common = ip;

            if (opcode == 3) {
                PEF_INPUT(count);
                ip += count;
            }

            for (size_t i = 0; i < repeats; i++) {
                PEF_SPACE(count);
                if (opcode == 3)
                    memcpy(out.p + op, packed.p + common, count);
                op += count;

                PEF_INPUT(custom);
                PEF_SPACE(custom);
                memcpy(out.p + op, packed.p + ip, custom);
                ip += custom;
                op += custom;
            }

            PEF_SPACE(count);
            if (opcode == 3)
                memcpy(out.p + op, packed.p + common, count);
            op += count;
        } else
            die("reserved PEF packed-data opcode");
    }

#undef PEF_INPUT
#undef PEF_SPACE

    return out;
}

bool pef_table(Buffer data, uint8_t table[256]) {
    bool found = false;

    for (size_t base = 0; base + 40 <= data.n; base++) {
        if (memcmp(data.p + base, "Joy!peff", 8))
            continue;

        unsigned sections = be16(data, base + 32);

        if (!sections || sections > 64 || !span(data, base + 40, (size_t)sections * 28))
            continue;

        for (unsigned i = 0; i < sections; i++) {
            size_t header = base + 40 + (size_t)i * 28;

            if (data.p[header + 24] != 2)
                continue;

            size_t expanded = be32(data, header + 12);
            size_t packed_size = be32(data, header + 16);
            size_t packed_offset = be32(data, header + 20);

            if (!span(data, base + packed_offset, packed_size))
                die("PEF packed-data section outside installer");

            Buffer unpacked = unpack_pef_data(slice(data, base + packed_offset, packed_size),
                                              expanded);
            uint8_t candidate[256];
            unsigned matches = find_permutation(unpacked, candidate);

            free(unpacked.p);

            if (matches > 1)
                die("PEF packed-data section has multiple substitution-table candidates");
            if (matches == 1) {
                if (found && memcmp(table, candidate, sizeof(candidate)))
                    die("PEF applications contain different substitution tables");
                memcpy(table, candidate, sizeof(candidate));
                found = true;
            }
        }
    }

    return found;
}

void data0_table(Buffer d, uint8_t table[256]) {
    /*
     * Expanded DATA 0 is three small initialization programs. Opcodes 1-4
     * encode relocation templates; the high nibble selects literal, skip,
     * fill, or 0xff-fill runs. Values 5-15 are reserved and rejected.
     * The 64 KiB arrays model signed 16-bit A5-relative addresses; adding
     * 32768 maps -32768..32767 to array indices. The substitution table moves
     * between VISE releases. The original 68K code addresses it through a
     * build-specific A5-relative global; DATA 0 has no field naming that
     * global. Identify the initializer's sole contiguous 256-byte permutation
     * instead of interpreting each installer's machine code.
     */
    uint8_t mem[65536] = {0}, set[65536] = {0};
    size_t src = 4;
    for (int section = 0; section < 3; section++) {
        int32_t dst = (int32_t)be32(d, src);
        src += 4;
#define STORE(V)                                                                                   \
    do {                                                                                           \
        unsigned ix = (uint16_t)(dst + 32768);                                                     \
        mem[ix] = (uint8_t)(V);                                                                    \
        set[ix] = 1;                                                                               \
        dst++;                                                                                     \
    } while (0)
#define COPY(N)                                                                                    \
    do {                                                                                           \
        unsigned nn = (N);                                                                         \
        while (nn--) {                                                                             \
            if (!span(d, src, 1))                                                                  \
                die("truncated DATA 0 literal");                                                   \
            STORE(d.p[src++]);                                                                     \
        }                                                                                          \
    } while (0)
        for (;;) {
            if (!span(d, src, 1))
                die("truncated DATA 0 command");
            unsigned c = d.p[src++];
            if (!c)
                break;
            if (c == 1) {
                dst += 4;
                STORE(0xff);
                STORE(0xff);
                COPY(2);
            } else if (c == 2) {
                dst += 4;
                STORE(0xff);
                COPY(3);
            } else if (c == 3) {
                STORE(0xa9);
                STORE(0xf0);
                dst += 2;
                COPY(2);
                dst++;
                COPY(1);
            } else if (c == 4) {
                STORE(0xa9);
                STORE(0xf0);
                dst++;
                COPY(3);
                dst++;
                COPY(1);
            } else if (c & 0x80)
                COPY((c & 0x7f) + 1);
            else if (c & 0x40)
                dst += (c & 0x3f) + 1;
            else if (c & 0x20) {
                if (!span(d, src, 1))
                    die("truncated DATA 0 fill");
                unsigned n = (c & 31) + 2, v = d.p[src++];
                while (n--)
                    STORE(v);
            } else if (c & 0x10) {
                unsigned n = (c & 15) + 1;
                while (n--)
                    STORE(0xff);
            } else
                die("unknown DATA 0 initializer command");
        }
#undef COPY
#undef STORE
    }
    unsigned matches = 0;

    for (unsigned start = 0; start <= 65536 - 256; start++)
        if (set[start] && find_permutation((Buffer){mem + start, 256}, table) == 1) {
            bool all_set = true;

            for (unsigned i = 0; i < 256; i++)
                all_set = all_set && set[start + i];

            if (all_set)
                matches++;
        }

    if (matches != 1)
        die("could not identify a unique VISE substitution table");
}
