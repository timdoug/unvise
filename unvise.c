#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>

/*
 * unvise decodes the InstallerVISE layer of a classic Macintosh installer.
 * MacBinary, BinHex, and StuffIt are transport/archive formats outside its
 * scope; remove them first with unar or macutils while preserving both forks.
 *
 * The data fork contains the SVCT catalog and compressed file payloads. The
 * resource fork contains DATA and CODE resources required to initialize the
 * decompressor, so a data fork alone is generally not sufficient.
 *
 * The implementation is verified against 25 freeware, shareware, demo, and
 * InstallerVISE self-installers made with Lite 3.6 and versions 4.2 through
 * 7.0. This is still a reverse-engineered implementation rather than a full
 * vendor specification: password-protected members are unsupported, catalog
 * record boundaries are inferred, and Active Install external payloads are
 * not understood.
 */

typedef struct {
    uint8_t *p;
    size_t n;
} Buffer;

typedef struct {
    size_t off, end;
    char tag[5];
    char *name;
    bool file;
    uint32_t parent, dir_id, payload;
    uint32_t packed[2], expanded[2];
    uint32_t gap;
} Record;

typedef struct {
    bool list, native, appledouble, raw_names;
    const char *out;
} Options;

static void die(const char *message) {
    fprintf(stderr, "unvise: %s\n", message);
    exit(1);
}

static void die_errno(const char *path) {
    fprintf(stderr, "unvise: %s: %s\n", path, strerror(errno));
    exit(1);
}

static void die_missing_resource_fork(void) {
    die("input is an InstallerVISE data fork, but its resource fork is missing; "
        "unpack with 'unar -k hidden' or 'macunpack -f' and keep both forks together");
}

static bool span(Buffer b, size_t off, size_t n) {
    return off <= b.n && n <= b.n - off;
}

static uint16_t be16(Buffer b, size_t off) {
    if (!span(b, off, 2))
        die("truncated 16-bit field");
    return (uint16_t)((b.p[off] << 8) | b.p[off + 1]);
}

static uint32_t be32(Buffer b, size_t off) {
    if (!span(b, off, 4))
        die("truncated 32-bit field");
    return ((uint32_t)b.p[off] << 24) | ((uint32_t)b.p[off + 1] << 16) |
           ((uint32_t)b.p[off + 2] << 8) | b.p[off + 3];
}

static Buffer slice(Buffer b, size_t off, size_t n) {
    if (!span(b, off, n))
        die("slice outside input");
    return (Buffer){b.p + off, n};
}

static Buffer read_file(const char *path) {
    FILE *f = fopen(path, "rb");

    if (!f)
        die_errno(path);
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0)
        die_errno(path);

    long length = ftell(f);
    rewind(f);

    uint8_t *p = malloc(length ? (size_t)length : 1);

    if (!p)
        die("out of memory");
    if ((size_t)length != fread(p, 1, (size_t)length, f))
        die_errno(path);
    if (fclose(f))
        die_errno(path);

    return (Buffer){p, (size_t)length};
}

static char *data_fork_path(const char *path) {
    static const char data_suffix[] = ".data";
    static const char resource_suffix[] = ".rsrc";
    size_t n = strlen(path);
    struct stat st;

    if (n >= sizeof(resource_suffix) - 1 &&
        !strcmp(path + n - (sizeof(resource_suffix) - 1), resource_suffix)) {
        size_t stem = n - (sizeof(resource_suffix) - 1);
        char *data = malloc(stem + sizeof(data_suffix));

        if (!data)
            die("out of memory");
        memcpy(data, path, stem);
        memcpy(data + stem, data_suffix, sizeof(data_suffix));
        return data;
    }

    if (!stat(path, &st)) {
        char *copy = strdup(path);

        if (!copy)
            die("out of memory");
        return copy;
    }
    if (errno != ENOENT)
        die_errno(path);

    char *data = malloc(n + sizeof(data_suffix));

    if (!data)
        die("out of memory");
    strcpy(data, path);
    strcat(data, data_suffix);
    return data;
}

static void append_byte(Buffer *b, size_t *capacity, uint8_t value) {
    if (b->n == *capacity) {
        if (*capacity > SIZE_MAX / 2)
            die("decoded data is too large");

        size_t next = *capacity ? *capacity * 2 : 4096;
        uint8_t *p = realloc(b->p, next);

        if (!p)
            die("out of memory");

        b->p = p;
        *capacity = next;
    }

    b->p[b->n++] = value;
}

static void write_file(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f)
        die_errno(path);
    if (n != fwrite(p, 1, n, f) || fclose(f))
        die_errno(path);
}

static void touch_file(const char *path) {
    FILE *f = fopen(path, "ab");

    if (!f)
        die_errno(path);
    if (fclose(f))
        die_errno(path);
}

#ifdef __APPLE__
static bool read_native_resource_fork(const char *path, Buffer *resource) {
    static const char suffix[] = "/..namedfork/rsrc";
    char *resource_path = malloc(strlen(path) + sizeof(suffix));

    if (!resource_path)
        die("out of memory");

    strcpy(resource_path, path);
    strcat(resource_path, suffix);

    struct stat st;

    if (stat(resource_path, &st)) {
        int error = errno;

        free(resource_path);

        if (error == ENOENT)
            return false;

        errno = error;
        die_errno(path);
    }

    *resource = read_file(resource_path);
    free(resource_path);

    return true;
}
#endif

static bool read_appledouble(const char *path, Buffer *resource) {
    struct stat st;

    if (stat(path, &st)) {
        if (errno == ENOENT)
            return false;
        die_errno(path);
    }

    Buffer sidecar = read_file(path);

    /*
     * AppleDouble keeps metadata in a 26-byte header followed by 12-byte
     * descriptors. Entry ID 2 is the raw Macintosh resource fork. The
     * standard loose-file convention stores this container as `._name`.
     */
    if (!span(sidecar, 0, 26) || be32(sidecar, 0) != UINT32_C(0x00051607)) {
        free(sidecar.p);
        return false;
    }

    unsigned count = be16(sidecar, 24);

    if (!span(sidecar, 26, (size_t)count * 12))
        die("truncated AppleDouble entry table");

    for (unsigned i = 0; i < count; i++) {
        size_t entry = 26 + (size_t)i * 12;

        if (be32(sidecar, entry) == 2) {
            uint32_t offset = be32(sidecar, entry + 4);
            uint32_t length = be32(sidecar, entry + 8);

            if (!span(sidecar, offset, length))
                die("truncated AppleDouble resource fork");

            resource->p = malloc(length ? length : 1);
            resource->n = length;
            if (!resource->p)
                die("out of memory");
            memcpy(resource->p, sidecar.p + offset, length);
            free(sidecar.p);
            return true;
        }
    }

    free(sidecar.p);
    return false;
}

static bool read_sidecar_resource_fork(const char *path, Buffer *resource) {
    /*
     * Look for the portable layouts emitted by the recommended front ends:
     *
     *     Installer + ._Installer       unar -k hidden
     *     Installer.data +
     *         Installer.rsrc           macunpack -f (raw forks)
     */
    const char *name = strrchr(path, '/');
    size_t directory = name ? (size_t)(name - path + 1) : 0;

    name = name ? name + 1 : path;

    size_t hidden_size = directory + 2 + strlen(name) + 1;
    char *hidden = malloc(hidden_size);

    if (!hidden)
        die("out of memory");

    memcpy(hidden, path, directory);
    snprintf(hidden + directory, hidden_size - directory, "._%s", name);

    bool found = read_appledouble(hidden, resource);
    free(hidden);

    if (found)
        return true;

    static const char data_suffix[] = ".data";
    static const char resource_suffix[] = ".rsrc";
    size_t path_length = strlen(path);

    if (path_length < sizeof(data_suffix) - 1 ||
        strcmp(path + path_length - (sizeof(data_suffix) - 1), data_suffix))
        return false;

    size_t stem_length = path_length - (sizeof(data_suffix) - 1);
    char *raw = malloc(stem_length + sizeof(resource_suffix));

    if (!raw)
        die("out of memory");

    memcpy(raw, path, stem_length);
    memcpy(raw + stem_length, resource_suffix, sizeof(resource_suffix));

    struct stat st;

    if (stat(raw, &st)) {
        if (errno != ENOENT)
            die_errno(raw);
        free(raw);
        return false;
    }

    *resource = read_file(raw);
    free(raw);

    return true;
}

static void mkdir_one(const char *path) {
    if (mkdir(path, 0777) && errno != EEXIST)
        die_errno(path);
}

static void mkdirs(const char *path) {
    char *copy = strdup(path);
    if (!copy)
        die("out of memory");
    for (char *p = copy + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir_one(copy);
            *p = '/';
        }
    mkdir_one(copy);
    free(copy);
}

static void make_parent_dir(const char *path) {
    char *copy = strdup(path), *slash;
    if (!copy)
        die("out of memory");
    slash = strrchr(copy, '/');
    if (slash) {
        *slash = 0;
        if (*copy)
            mkdirs(copy);
    }
    free(copy);
}

static bool resource_find(Buffer r, const char type[4], int wanted, Buffer *result) {
    uint32_t db = be32(r, 0), mb = be32(r, 4), ml = be32(r, 12);
    if (!span(r, mb, ml) || !span(r, mb + 24, 4))
        die("bad resource map");
    size_t tl = mb + be16(r, mb + 24);
    unsigned tc = (unsigned)be16(r, tl) + 1;
    for (unsigned ti = 0; ti < tc; ti++) {
        size_t te = tl + 2 + ti * 8;
        if (!span(r, te, 8))
            die("truncated resource type list");
        if (memcmp(r.p + te, type, 4))
            continue;
        unsigned count = (unsigned)be16(r, te + 4) + 1;
        size_t refs = tl + be16(r, te + 6);
        for (unsigned i = 0; i < count; i++) {
            size_t ref = refs + i * 12;
            if (!span(r, ref, 12))
                die("truncated resource reference");
            int id = (int16_t)be16(r, ref);
            if (id != wanted)
                continue;
            uint32_t rel =
                ((uint32_t)r.p[ref + 5] << 16) | ((uint32_t)r.p[ref + 6] << 8) | r.p[ref + 7];
            uint32_t n = be32(r, db + rel);
            *result = slice(r, db + rel + 4, n);
            return true;
        }
    }
    return false;
}

static void put_word(Buffer out, size_t *op, const uint8_t *word) {
    if (*op >= out.n)
        return;
    out.p[(*op)++] = word[0];
    if (*op < out.n)
        out.p[(*op)++] = word[1];
}

static Buffer unpack_code(Buffer p, Buffer code) {
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

static unsigned find_permutation(Buffer b, uint8_t table[256]) {
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

static void data0_table(Buffer d, uint8_t table[256]) {
    /*
     * Expanded DATA 0 is three small initialization programs interpreted by
     * the original installer. We emulate only their observed command set.
     * The 64 KiB arrays model signed 16-bit A5-relative addresses; adding
     * 32768 maps -32768..32767 to array indices. The substitution table moves
     * between VISE releases, but is identifiable as the initializer's sole
     * contiguous 256-byte permutation.
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

static Buffer inflate_stored(const uint8_t *x, size_t n, size_t expected) {
    /*
     * VISE stored blocks resemble DEFLATE stored blocks but align and order
     * data as big-endian 16-bit words. zlib cannot consume this representation
     * directly, so only this nonstandard block type is decoded here.
     */
    Buffer out = {(uint8_t *)malloc(expected ? expected : 1), 0};
    if (!out.p)
        die("out of memory");
    size_t p = 0;
    for (;;) {
        if (p + 6 > n)
            die("truncated VISE stored block");
        unsigned ctl = (x[p] << 8) | x[p + 1], len = (x[p + 2] << 8) | x[p + 3],
                 inv = (x[p + 4] << 8) | x[p + 5];
        p += 6;
        if (((ctl >> 1) & 3) || len != ((~inv) & 0xffffu))
            die("invalid VISE stored block");
        if (len > expected - out.n || p + ((len + 1) & ~1u) > n)
            die("VISE stored block overflow");
        for (unsigned i = 0; i < len; i++)
            out.p[out.n++] = (i & 1) ? x[p + i - 1] : x[p + i + 1];
        p += (len + 1) & ~1u;
        if (ctl & 1)
            break;
    }
    if (out.n != expected)
        die("expanded stored-block size mismatch");
    return out;
}

typedef struct {
    Buffer input;
    size_t bit;
} ViseBits;

typedef struct {
    Buffer output;
    size_t capacity;
    unsigned bit;
} DeflateBits;

typedef struct {
    unsigned count[16];
    unsigned symbol[288];
} Huffman;

static unsigned vise_bit(ViseBits *in) {
    size_t word = (in->bit / 16) * 2;
    unsigned shift = (unsigned)(in->bit % 16);

    if (!span(in->input, word, 2))
        die("truncated VISE DEFLATE bitstream");

    unsigned value = ((unsigned)in->input.p[word] << 8) | in->input.p[word + 1];

    in->bit++;
    return (value >> shift) & 1;
}

static void deflate_bit(DeflateBits *out, unsigned value) {
    if (!out->bit)
        append_byte(&out->output, &out->capacity, 0);

    out->output.p[out->output.n - 1] |= (uint8_t)((value & 1) << out->bit);
    out->bit = (out->bit + 1) & 7;
}

static unsigned copy_bits(ViseBits *in, DeflateBits *out, unsigned count) {
    unsigned value = 0;

    for (unsigned i = 0; i < count; i++) {
        unsigned bit = vise_bit(in);

        deflate_bit(out, bit);
        value |= bit << i;
    }

    return value;
}

static unsigned read_bits(ViseBits *in, unsigned count) {
    unsigned value = 0;

    for (unsigned i = 0; i < count; i++)
        value |= vise_bit(in) << i;

    return value;
}

static void write_bits(DeflateBits *out, unsigned value, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        deflate_bit(out, value >> i);
}

static void huffman_make(Huffman *h, const uint8_t *lengths, unsigned count) {
    memset(h, 0, sizeof(*h));

    for (unsigned i = 0; i < count; i++) {
        if (lengths[i] > 15)
            die("invalid VISE Huffman code length");
        h->count[lengths[i]]++;
    }

    unsigned offsets[16] = {0};

    for (unsigned length = 1; length < 15; length++)
        offsets[length + 1] = offsets[length] + h->count[length];

    for (unsigned symbol = 0; symbol < count; symbol++)
        if (lengths[symbol])
            h->symbol[offsets[lengths[symbol]]++] = symbol;
}

static unsigned huffman_symbol(ViseBits *in, DeflateBits *out, const Huffman *h) {
    unsigned code = 0, first = 0, index = 0;

    for (unsigned length = 1; length <= 15; length++) {
        code |= copy_bits(in, out, 1);
        unsigned count = h->count[length];

        if (code < first + count)
            return h->symbol[index + code - first];

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }

    die("invalid VISE Huffman code");
    return 0;
}

static void fixed_huffman(Huffman *literal, Huffman *distance) {
    uint8_t lengths[288];
    uint8_t distances[32];

    for (unsigned i = 0; i < 288; i++)
        lengths[i] = i < 144 ? 8 : (i < 256 ? 9 : (i < 280 ? 7 : 8));
    memset(distances, 5, sizeof(distances));
    huffman_make(literal, lengths, 288);
    huffman_make(distance, distances, 32);
}

static void dynamic_huffman(ViseBits *in, DeflateBits *out, Huffman *literal,
                            Huffman *distance) {
    static const uint8_t order[19] = {16, 17, 18, 0,  8, 7, 9,  6, 10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};
    unsigned literal_count = copy_bits(in, out, 5) + 257;
    unsigned distance_count = copy_bits(in, out, 5) + 1;
    unsigned code_count = copy_bits(in, out, 4) + 4;
    uint8_t code_lengths[19] = {0};

    for (unsigned i = 0; i < code_count; i++)
        code_lengths[order[i]] = (uint8_t)copy_bits(in, out, 3);

    Huffman code;
    huffman_make(&code, code_lengths, 19);

    uint8_t lengths[320] = {0};
    unsigned total = literal_count + distance_count;
    unsigned n = 0;

    while (n < total) {
        unsigned symbol = huffman_symbol(in, out, &code);

        if (symbol < 16) {
            lengths[n++] = (uint8_t)symbol;
            continue;
        }

        unsigned repeat = 0, value = 0;

        if (symbol == 16) {
            if (!n)
                die("VISE Huffman repeat has no previous length");
            value = lengths[n - 1];
            repeat = copy_bits(in, out, 2) + 3;
        } else if (symbol == 17)
            repeat = copy_bits(in, out, 3) + 3;
        else if (symbol == 18)
            repeat = copy_bits(in, out, 7) + 11;
        else
            die("invalid VISE Huffman repeat");

        if (repeat > total - n)
            die("VISE Huffman repeat overflow");

        while (repeat--)
            lengths[n++] = (uint8_t)value;
    }

    huffman_make(literal, lengths, literal_count);
    huffman_make(distance, lengths + literal_count, distance_count);
}

static void compressed_block(ViseBits *in, DeflateBits *out, const Huffman *literal,
                             const Huffman *distance) {
    static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                              1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                              4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
                                                4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                                9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        unsigned symbol = huffman_symbol(in, out, literal);

        if (symbol < 256)
            continue;
        if (symbol == 256)
            return;
        if (symbol > 285)
            die("invalid VISE length symbol");

        copy_bits(in, out, length_extra[symbol - 257]);
        unsigned dist = huffman_symbol(in, out, distance);

        if (dist >= 30)
            die("invalid VISE distance symbol");
        copy_bits(in, out, distance_extra[dist]);
    }
}

static Buffer standardize_deflate(Buffer transformed) {
    ViseBits in = {transformed, 0};
    DeflateBits out = {0};
    bool final;

    do {
        final = copy_bits(&in, &out, 1) != 0;
        unsigned type = copy_bits(&in, &out, 2);

        if (type == 0) {
            while (in.bit & 15)
                vise_bit(&in);
            while (out.bit)
                deflate_bit(&out, 0);

            unsigned length = read_bits(&in, 16);
            unsigned inverse = read_bits(&in, 16);

            if (length != ((~inverse) & 0xffffu))
                die("invalid VISE stored block lengths");

            write_bits(&out, length, 16);
            write_bits(&out, inverse, 16);

            for (unsigned i = 0; i < length; i++)
                write_bits(&out, read_bits(&in, 8), 8);

            if (length & 1)
                read_bits(&in, 8);
        } else if (type == 1 || type == 2) {
            Huffman literal, distance;

            if (type == 1)
                fixed_huffman(&literal, &distance);
            else
                dynamic_huffman(&in, &out, &literal, &distance);

            compressed_block(&in, &out, &literal, &distance);
        } else
            die("invalid VISE DEFLATE block type");
    } while (!final);

    return out.output;
}

static Buffer inflate_member(Buffer packed, const uint8_t table[256], size_t expected) {
    if (packed.n < 2)
        die("truncated VISE member");

    uint8_t *x = malloc(packed.n);

    if (!x)
        die("out of memory");

    /* Every member byte first passes through DATA 0's substitution table. */
    for (size_t i = 0; i < packed.n; i++)
        x[i] = table[packed.p[i]];

    unsigned first = (x[0] << 8) | x[1];

    if (((first >> 1) & 3) == 0) {
        Buffer out = inflate_stored(x, packed.n, expected);
        free(x);
        return out;
    }

    Buffer deflate = standardize_deflate((Buffer){x, packed.n});

    if (deflate.n > UINT_MAX || expected >= UINT_MAX)
        die("compressed member is too large for zlib");

    Buffer out = {(uint8_t *)malloc(expected + 1), expected};

    if (!out.p)
        die("out of memory");

    z_stream z = {0};
    z.next_in = deflate.p;
    z.avail_in = (uInt)deflate.n;
    z.next_out = out.p;
    z.avail_out = (uInt)(expected + 1);

    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        die("cannot initialize zlib");

    int status = inflate(&z, Z_FINISH);

    bool exhausted_at_declared_size =
        status == Z_BUF_ERROR && z.avail_in == 0 && z.total_out == expected;

    if ((status != Z_STREAM_END && !exhausted_at_declared_size) || z.total_out != expected) {
        fprintf(stderr,
                "unvise: inflate failed (%d, in=%lu+%u, out=%lu/0x%zx bytes%s%s)\n", status,
                z.total_in, z.avail_in, z.total_out, expected, z.msg ? ": " : "",
                z.msg ? z.msg : "");
        exit(1);
    }

    inflateEnd(&z);
    free(deflate.p);
    free(x);

    return out;
}

static Buffer inflate_catalog(Buffer data, size_t catalog) {
    size_t packed_size = be32(data, catalog + 4);
    Buffer packed = slice(data, catalog + 0x64, packed_size);

    if (packed.n > UINT_MAX)
        die("packed catalog is too large for zlib");

    uint8_t *input = malloc(packed.n ? packed.n : 1);

    if (!input)
        die("out of memory");

    for (size_t i = 0; i < packed.n; i += 2) {
        if (i + 1 == packed.n)
            input[i] = packed.p[i];
        else {
            input[i] = packed.p[i + 1];
            input[i + 1] = packed.p[i];
        }
    }

    if (packed.n > SIZE_MAX / 4)
        die("packed catalog is too large");

    size_t capacity = packed.n < 4096 ? 4096 : packed.n * 4;
    Buffer out = {malloc(capacity), 0};

    if (!out.p)
        die("out of memory");

    z_stream z = {0};
    z.next_in = input;
    z.avail_in = (uInt)packed.n;

    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        die("cannot initialize zlib for packed catalog");

    int status;

    do {
        if (z.total_out == capacity) {
            if (capacity > SIZE_MAX / 2)
                die("packed catalog is too large");
            capacity *= 2;
            out.p = realloc(out.p, capacity);
            if (!out.p)
                die("out of memory");
        }

        z.next_out = out.p + z.total_out;
        z.avail_out = (uInt)(capacity - z.total_out > UINT_MAX ? UINT_MAX : capacity - z.total_out);
        status = inflate(&z, Z_NO_FLUSH);
    } while (status == Z_OK);

    if (status != Z_STREAM_END)
        die("could not inflate packed catalog");

    out.n = z.total_out;
    inflateEnd(&z);
    free(input);

    return out;
}

static void utf8_add(char **q, uint32_t c) {
    if (c < 0x80)
        *(*q)++ = (char)c;
    else if (c < 0x800) {
        *(*q)++ = (char)(0xc0 | (c >> 6));
        *(*q)++ = (char)(0x80 | (c & 63));
    } else {
        *(*q)++ = (char)(0xe0 | (c >> 12));
        *(*q)++ = (char)(0x80 | ((c >> 6) & 63));
        *(*q)++ = (char)(0x80 | (c & 63));
    }
}

static uint32_t macroman(unsigned c) {
    static const uint16_t unicode[128] = {
        0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1,
        0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
        0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3,
        0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
        0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df,
        0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
        0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211,
        0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x2126, 0x00e6, 0x00f8,
        0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab,
        0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
        0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca,
        0x00ff, 0x0178, 0x2044, 0x00a4, 0x2039, 0x203a, 0xfb01, 0xfb02,
        0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1,
        0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
        0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc,
        0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7,
    };

    if (c < 128)
        return c;
    return unicode[c - 128];
}

static char *convert_name(const uint8_t *p, size_t n, bool raw) {
    char *s = malloc((raw ? n : n * 3) + 1), *q = s;

    if (!s)
        die("out of memory");

    if (raw) {
        memcpy(s, p, n);
        q += n;
    } else
        for (size_t i = 0; i < n; i++)
            utf8_add(&q, macroman(p[i]));

    *q = 0;
    return s;
}

static char *record_name(Buffer data, Record *r, bool packed_catalog, bool old_catalog,
                         bool raw_names) {
    if (old_catalog && (!strcmp(r->tag, "FVCT") || !strcmp(r->tag, "DVCT"))) {
        size_t trailer = !strcmp(r->tag, "FVCT") ? 2 : 8;

        for (size_t p = r->off + 4; p + trailer <= r->end; p++) {
            size_t n = data.p[p];

            if (p + trailer + n != r->end || (!strcmp(r->tag, "FVCT") && data.p[p + 1] != 0x2c))
                continue;

            return convert_name(data.p + p + trailer, n, raw_names);
        }

        return NULL;
    }

    /* VISE 6.5 packed catalogs add four bytes before FVCT and DVCT names. */
    size_t start = !strcmp(r->tag, "FVCT")
                       ? (packed_catalog ? 0xbe : 0xba)
                       : (!strcmp(r->tag, "DVCT") ? (packed_catalog ? 0x98 : 0x94) : 0);
    if (!start || r->off + start >= r->end)
        return NULL;
    size_t n = r->end - (r->off + start);
    return convert_name(data.p + r->off + start, n, raw_names);
}

static void print_quoted(const char *s, bool raw) {
    putchar('"');

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c < 0x20 || c == 0x7f || (raw && c >= 0x80))
            printf("\\x%02X", c);
        else if (c == '"' || c == '\\')
            printf("\\%c", c);
        else
            putchar(c);
    }

    putchar('"');
}

static bool is_tag(const uint8_t *p) {
    return !memcmp(p, "CVCT", 4) || !memcmp(p, "DVCT", 4) || !memcmp(p, "FVCT", 4) ||
           !memcmp(p, "PACK", 4);
}

static Record *catalog(Buffer d, size_t cat, bool packed_catalog, bool old_catalog, bool raw_names,
                       size_t *count) {
    /*
     * No authoritative record-length field has been identified. Boundaries
     * are therefore inferred by scanning for the four observed record tags.
     * This works for the verified archive but could mistake tag-like bytes in
     * an unknown catalog variant for a new record.
     */
    size_t cap = 32, n = 0;
    Record *r = calloc(cap, sizeof(*r));
    if (!r)
        die("out of memory");
    for (size_t p = cat; p + 4 <= d.n;) {
        while (p + 4 <= d.n && !is_tag(d.p + p))
            p++;
        if (p + 4 > d.n)
            break;
        if (n == cap) {
            cap *= 2;
            r = realloc(r, cap * sizeof(*r));
            if (!r)
                die("out of memory");
            memset(r + n, 0, (cap - n) * sizeof(*r));
        }
        r[n].off = p;
        memcpy(r[n].tag, d.p + p, 4);
        r[n].tag[4] = 0;
        n++;
        p += 4;
    }
    for (size_t i = 0; i < n; i++) {
        r[i].end = i + 1 < n ? r[i + 1].off : d.n;
        r[i].name = record_name(d, &r[i], packed_catalog, old_catalog, raw_names);
        if (!strcmp(r[i].tag, "DVCT") && r[i].end - r[i].off >= 0x24) {
            r[i].dir_id = be32(d, r[i].off + 0x1c);
            r[i].parent = be32(d, r[i].off + 0x20);
        }
        if (!strcmp(r[i].tag, "FVCT") && r[i].end - r[i].off >= 0x68) {
            /*
             * FVCT also represents installer actions. Observed folder-search
             * actions use type 0x03xxxxxx and message/location actions use
             * 0x00008000. Other nonzero values are ordinary file flags.
             */
            r[i].packed[0] = be32(d, r[i].off + 0x44);
            r[i].expanded[0] = be32(d, r[i].off + 0x48);
            r[i].packed[1] = be32(d, r[i].off + 0x4c);
            r[i].expanded[1] = be32(d, r[i].off + 0x50);
            r[i].payload = be32(d, r[i].off + 0x64);

            /*
             * Search/delete and similar actions reuse these fields for
             * parameters. This stable sentinel distinguishes them from fork
             * lengths in both the short VISE 4.2 and later catalog layouts.
             */
            bool parameter_record = (r[i].packed[0] == UINT32_C(0x00010001) ||
                                     r[i].packed[0] == UINT32_C(0x00020001) ||
                                     r[i].packed[0] == UINT32_C(0x00040001)) &&
                                    r[i].packed[1] == UINT32_C(0x00010001);

            if (old_catalog) {
                r[i].file = !parameter_record &&
                            (r[i].packed[0] || r[i].packed[1] || r[i].expanded[0] ||
                             r[i].expanded[1]);
                r[i].parent = be32(d, r[i].off + 0x58);
            } else {
                uint32_t type = be32(d, r[i].off + 4);

                r[i].file = !parameter_record &&
                            (type == 0 || type >= UINT32_C(0x00010000)) && (type >> 24) != 3;
                r[i].parent = be32(d, r[i].off + 0x58);

                /* VISE 7 version-source records carry the intervening source size here. */
                if (r[i].end - r[i].off >= 0x70 &&
                    !memcmp(d.p + r[i].off + 0x2c, "issp", 4) && be32(d, r[i].off + 0x68) &&
                    be32(d, r[i].off + 0x6c))
                    r[i].gap = be32(d, r[i].off + 0x68);
            }
        }
    }

    if (old_catalog) {
        uint32_t current_dir = 0;

        for (size_t i = 0; i < n; i++) {
            if (!strcmp(r[i].tag, "DVCT")) {
                current_dir = r[i].dir_id;
                continue;
            }

            if (strcmp(r[i].tag, "FVCT") || !r[i].file)
                continue;

            bool known_parent = false;

            for (size_t j = 0; j < n; j++)
                known_parent = known_parent ||
                               (!strcmp(r[j].tag, "DVCT") && r[j].dir_id == r[i].parent);

            /*
             * Lite 3.6 sometimes puts an install-location token in FVCT+58
             * instead of the containing DVCT ID. Catalog order retains that
             * association. A value matching a DVCT remains authoritative;
             * the common root ID is the parent of the catalog directories.
             */
            if (!known_parent) {
                bool root_parent = false;

                for (size_t j = 0; j < n; j++)
                    root_parent = root_parent ||
                                  (!strcmp(r[j].tag, "DVCT") && r[j].parent == r[i].parent);

                if (root_parent)
                    current_dir = 0;
                else if (current_dir)
                    r[i].parent = current_dir;
            }
        }
    }
    *count = n;
    return r;
}

static char *safe_name(const char *name, bool flat) {
    size_t n = strlen(name);
    char *s = malloc(n + 32), *q = s;
    if (!s)
        die("out of memory");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0)
            continue;
        if (flat && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-')) {
            if (q == s || q[-1] != '_')
                *q++ = '_';
        } else
            *q++ = (c == '/' ? ':' : (char)c);
    }
    while (flat && q > s && q[-1] == '_')
        q--;
    *q = 0;
    if (!*s)
        strcpy(s, "unnamed");
    if (!strcmp(s, ".") || !strcmp(s, "..")) {
        memmove(s + 1, s, strlen(s) + 1);
        s[0] = '_';
    }
    return s;
}

static Record *dir_by_id(Record *r, size_t n, uint32_t id) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(r[i].tag, "DVCT") && r[i].dir_id == id)
            return &r[i];
    return NULL;
}

static char *output_path(const Options *o, Record *all, size_t count, size_t index,
                         const char *fork) {
    Record *r = &all[index];
    char *name = safe_name(r->name ? r->name : "unnamed", false);
    size_t cap = strlen(o->out) + strlen(name) + 128;
    char **parts = calloc(count, sizeof(char *));
    size_t np = 0;
    uint32_t parent = r->parent;
    if (!parts)
        die("out of memory");
    for (size_t guard = 0; guard < count; guard++) {
        Record *d = dir_by_id(all, count, parent);
        if (!d)
            break;
        parts[np++] = safe_name(d->name ? d->name : "unnamed", false);
        parent = d->parent;
    }
    for (size_t i = 0; i < np; i++)
        cap += strlen(parts[i]) + 1;
    char *path = malloc(cap);
    if (!path)
        die("out of memory");
    strcpy(path, o->out);
    for (size_t i = np; i; i--) {
        strcat(path, "/");
        strcat(path, parts[i - 1]);
        free(parts[i - 1]);
    }
    strcat(path, "/");
    strcat(path, name);
    if (!o->native && !o->appledouble) {
        strcat(path, ".");
        strcat(path, fork);
    }
    make_parent_dir(path);
    free(parts);
    free(name);
    return path;
}

static void put_be16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static char *appledouble_path(const char *path) {
    const char *name = strrchr(path, '/');
    size_t directory = name ? (size_t)(name - path + 1) : 0;
    char *sidecar = malloc(strlen(path) + 3);

    if (!sidecar)
        die("out of memory");

    memcpy(sidecar, path, directory);
    strcpy(sidecar + directory, "._");
    strcpy(sidecar + directory + 2, path + directory);
    return sidecar;
}

static void write_appledouble(const char *path, const uint8_t *p, size_t n) {
    enum { header_size = 26, entry_size = 12, data_offset = header_size + entry_size };

    if (n > UINT32_MAX || n > SIZE_MAX - data_offset)
        die("resource fork is too large for AppleDouble");

    Buffer sidecar = {calloc(data_offset + n, 1), data_offset + n};

    if (!sidecar.p)
        die("out of memory");

    /* AppleDouble version 2 with one entry: ID 2 is the raw resource fork. */
    put_be32(sidecar.p, UINT32_C(0x00051607));
    put_be32(sidecar.p + 4, UINT32_C(0x00020000));
    put_be16(sidecar.p + 24, 1);
    put_be32(sidecar.p + 26, 2);
    put_be32(sidecar.p + 30, data_offset);
    put_be32(sidecar.p + 34, (uint32_t)n);
    memcpy(sidecar.p + data_offset, p, n);

    char *sidecar_path = appledouble_path(path);

    touch_file(path);
    write_file(sidecar_path, sidecar.p, sidecar.n);
    free(sidecar_path);
    free(sidecar.p);
}

static void write_output(const Options *o, const char *path, const char *fork, const uint8_t *p,
                         size_t n) {
    if (o->appledouble) {
        if (!strcmp(fork, "data"))
            write_file(path, p, n);
        else
            write_appledouble(path, p, n);
        return;
    }

    if (!o->native) {
        write_file(path, p, n);
        return;
    }

#ifdef __APPLE__
    if (!strcmp(fork, "data")) {
        write_file(path, p, n);
    } else {
        /* APFS/HFS expose a file's resource fork through this named-fork path. */
        static const char suffix[] = "/..namedfork/rsrc";
        char *resource_path = malloc(strlen(path) + sizeof(suffix));

        if (!resource_path)
            die("out of memory");

        touch_file(path);
        strcpy(resource_path, path);
        strcat(resource_path, suffix);
        write_file(resource_path, p, n);
        free(resource_path);
    }

#else
    (void)path;
    (void)fork;
    (void)p;
    (void)n;
    die("-n is only supported on macOS");
#endif
}

static void usage(FILE *f) {
    fprintf(f, "usage: unvise [-l] [-x DIR] [-a | -n] [-r] INSTALLER\n"
               "  -l        list files without extracting\n"
               "  -x DIR    extract files into DIR\n"
               "  -a        write ._name AppleDouble sidecars\n"
               "  -n        use native macOS resource forks\n"
               "  -r        preserve MacRoman filename bytes\n"
               "  -h        show this help\n");
}

int main(int argc, char **argv) {
    Options o = {0};
    const char *input_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l"))
            o.list = true;
        else if (!strcmp(argv[i], "-x") && i + 1 < argc)
            o.out = argv[++i];
        else if (!strcmp(argv[i], "-n"))
            o.native = true;
        else if (!strcmp(argv[i], "-a"))
            o.appledouble = true;
        else if (!strcmp(argv[i], "-r"))
            o.raw_names = true;
        else if (!strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] == '-') {
            usage(stderr);
            return 2;
        } else if (input_path) {
            usage(stderr);
            return 2;
        } else
            input_path = argv[i];
    }

    if (!input_path) {
        usage(stderr);
        return 2;
    }

    if (o.native && o.appledouble)
        die("-n and -a are mutually exclusive");

#ifndef __APPLE__
    if (o.native)
        die("-n is only supported on macOS");
#else
    if (o.raw_names && o.out)
        die("-r extraction is not supported by modern macOS filesystems");
#endif

    /*
     * At this boundary INSTALLER is the actual SVCT data fork, not the .bin,
     * .hqx, or .sit file in which it was downloaded. Its resource fork may be
     * native on macOS or stored in one of the portable sidecar layouts above.
     */
    char *resolved_path = data_fork_path(input_path);
    Buffer input = read_file(resolved_path);
    Buffer data = input, resource = {0};
    bool resource_owned = false;

    if (input.n < 4 || memcmp(input.p, "SVCT", 4))
        die("not an InstallerVISE data fork; unpack MacBinary, BinHex, or StuffIt first");

    bool found_resource = false;

#ifdef __APPLE__
    found_resource = read_native_resource_fork(resolved_path, &resource);
#endif
    if (!found_resource)
        found_resource = read_sidecar_resource_fork(resolved_path, &resource);
    if (!found_resource)
        die_missing_resource_fork();

    resource_owned = true;

    Buffer packed = {0}, code = {0}, data0 = {0};
    bool data0_owned = false;
    bool direct_table = false;

    if (!resource_find(resource, "DATA", 0, &packed))
        die("installer lacks DATA 0");

    if (packed.n >= 4 && be32(packed, 0) == UINT32_C(0xa89f000c)) {
        if (!resource_find(resource, "CODE", 24, &code) &&
            !resource_find(resource, "CODE", 23, &code))
            die("packed DATA 0 lacks its dictionary resource");

        if (code.n >= 4 && be32(code, 0) == UINT32_C(0xa89f000c)) {
            Buffer dictionary = {0};

            if (!resource_find(resource, "CODE", 25, &dictionary) &&
                !resource_find(resource, "CODE", 1002, &dictionary))
                die("compressed CODE 24 lacks its dictionary resource");

            /* Self-hosting VISE installers keep the DATA 0 dictionary here. */
            code = dictionary;
        }

        data0 = unpack_code(packed, code);
        data0_owned = true;
    } else {
        /* Installer VISE Lite 3.6 stores the substitution table in DATA 0. */
        data0 = packed;
        direct_table = true;
    }

    if (data.n < 0x28)
        die("truncated InstallerVISE SVCT data fork");

    uint32_t version = be32(data, 4), cat = be32(data, 0x24);

    if (cat >= data.n || !span(data, cat, 4) || memcmp(data.p + cat, "CVCT", 4))
        die("invalid SVCT catalog offset");

    printf("SVCT version=%" PRIu32 " size=%zu catalog=0x%X\n", version, data.n, cat);

    if (!o.list && !o.out)
        return 0;

    bool packed_catalog = be32(data, cat + 8) != 0;
    bool old_catalog = direct_table && !packed_catalog;
    Buffer catalog_data = data;
    size_t catalog_offset = cat;

    if (packed_catalog) {
        if (!span(data, cat + 0x14, 4) || memcmp(data.p + cat + 0x14, "PACK", 4))
            die("packed catalog lacks a PACK header");
        catalog_data = inflate_catalog(data, cat);
        catalog_offset = 0;
    }

    size_t count;
    Record *records =
        catalog(catalog_data, catalog_offset, packed_catalog, old_catalog, o.raw_names, &count);
    uint8_t table[256];

    if (o.out) {
        mkdirs(o.out);
        if (direct_table) {
            if (find_permutation(data0, table) != 1)
                die("could not identify a unique VISE substitution table");
        } else
            data0_table(data0, table);
    }

    bool *shared_done = calloc(count, sizeof(bool));

    if (!shared_done)
        die("out of memory");

    for (size_t i = 0; i < count; i++) {
        Record *r = &records[i];

        if (o.list) {
            printf("%04zu 0x%08zX %-4s size=0x%zX", i, r->off, r->tag, r->end - r->off);
            if (!strcmp(r->tag, "FVCT") && r->file)
                printf(" payload=0x%X data=0x%X->0x%X rsrc=0x%X->0x%X", r->payload, r->packed[0],
                       r->expanded[0], r->packed[1], r->expanded[1]);
            else if (!strcmp(r->tag, "FVCT"))
                fputs(" action", stdout);
            if (r->name) {
                fputs(" name=", stdout);
                print_quoted(r->name, o.raw_names);
            }
            putchar('\n');
        }

        if (!o.out || strcmp(r->tag, "FVCT") || !r->file)
            continue;

        if ((!r->expanded[0] && !r->expanded[1]) || (!r->packed[0] && !r->packed[1]))
            continue;

        if (r->gap) {
            size_t total = r->packed[1];
            size_t resource_start = (size_t)r->expanded[0] + r->gap;

            if (resource_start > total || r->expanded[1] > total - resource_start)
                die("version-source payload layout overflow");
            if (packed_catalog && r->payload >= cat)
                die("installer uses an external web payload archive; the stub alone cannot be "
                    "extracted");
            if ((size_t)r->payload + r->packed[0] > cat)
                die("payload overlaps catalog");

            Buffer expanded =
                inflate_member(slice(data, r->payload, r->packed[0]), table, total);

            if (r->expanded[0]) {
                char *path = output_path(&o, records, count, i, "data");

                write_output(&o, path, "data", expanded.p, r->expanded[0]);
                free(path);
            }

            if (r->expanded[1]) {
                char *path = output_path(&o, records, count, i, "rsrc");

                write_output(&o, path, "rsrc", expanded.p + resource_start, r->expanded[1]);
                free(path);
            }

            free(expanded.p);
            continue;
        }

        size_t shared_count = 0, first = i;

        for (size_t j = 0; j < count; j++)
            if (!strcmp(records[j].tag, "FVCT") && records[j].file &&
                (records[j].expanded[0] || records[j].expanded[1]) &&
                (records[j].packed[0] || records[j].packed[1]) &&
                records[j].payload == r->payload) {
                if (!shared_count)
                    first = j;
                shared_count++;
            }

        if (shared_count > 1) {
            /*
             * Some FVCT records share one compressed member. Combined-fork
             * groups store its compressed and total expanded sizes in packed
             * fields 0 and 1. Resource-only groups put the compressed size in
             * packed field 1. Fork contents are concatenated in catalog order,
             * with each record's data fork before its resource fork.
             */
            if (i != first || shared_done[first])
                continue;
            shared_done[first] = true;

            if (packed_catalog && r->payload >= cat)
                die("installer uses an external web payload archive; the stub alone cannot be "
                    "extracted");
            size_t expanded_size = 0;

            for (size_t j = 0; j < count; j++)
                if (!strcmp(records[j].tag, "FVCT") && records[j].file &&
                    (records[j].expanded[0] || records[j].expanded[1]) &&
                    (records[j].packed[0] || records[j].packed[1]) &&
                    records[j].payload == r->payload)
                    for (int f = 0; f < 2; f++) {
                        if (records[j].expanded[f] > SIZE_MAX - expanded_size)
                            die("shared payload is too large");
                        expanded_size += records[j].expanded[f];
                    }

            size_t packed_size = 0;

            if (r->packed[0] && r->packed[1] == expanded_size)
                packed_size = r->packed[0];
            else if (!!r->packed[0] != !!r->packed[1])
                packed_size = r->packed[0] ? r->packed[0] : r->packed[1];
            else {
                fprintf(stderr,
                        "unvise: unknown shared payload layout at 0x%X "
                        "(0x%X, 0x%X, expanded 0x%zx)\n",
                        r->payload, r->packed[0], r->packed[1], expanded_size);
                exit(1);
            }

            Buffer packed = slice(data, r->payload, packed_size);
            Buffer expanded = inflate_member(packed, table, expanded_size);
            size_t pos = 0;
            for (size_t j = 0; j < count; j++)
                if (!strcmp(records[j].tag, "FVCT") && records[j].file &&
                    (records[j].expanded[0] || records[j].expanded[1]) &&
                    (records[j].packed[0] || records[j].packed[1]) &&
                    records[j].payload == r->payload)
                    for (int f = 0; f < 2; f++)
                        if (records[j].expanded[f]) {
                            size_t n = records[j].expanded[f];
                            if (n > expanded.n - pos)
                                die("shared payload layout overflow");
                            char *path = output_path(&o, records, count, j, f ? "rsrc" : "data");
                            write_output(&o, path, f ? "rsrc" : "data", expanded.p + pos, n);
                            free(path);
                            pos += n;
                        }
            if (pos != expanded.n)
                die("unassigned bytes in shared payload");
            free(expanded.p);
            continue;
        }

        size_t pos = r->payload;

        for (int f = 0; f < 2; f++)
            if (r->packed[f]) {
                if (packed_catalog && pos >= cat)
                    die("installer uses an external web payload archive; the stub alone cannot be "
                        "extracted");
                if (pos + r->packed[f] > cat)
                    die("payload overlaps catalog");
                Buffer expanded =
                    inflate_member(slice(data, pos, r->packed[f]), table, r->expanded[f]);
                char *path = output_path(&o, records, count, i, f ? "rsrc" : "data");
                write_output(&o, path, f ? "rsrc" : "data", expanded.p, expanded.n);
                free(path);
                free(expanded.p);
                pos += r->packed[f];
            }
    }

    for (size_t i = 0; i < count; i++)
        free(records[i].name);

    free(records);
    free(shared_done);
    if (packed_catalog)
        free(catalog_data.p);
    if (data0_owned)
        free(data0.p);
    if (resource_owned)
        free(resource.p);
    free(input.p);
    free(resolved_path);

    return 0;
}
