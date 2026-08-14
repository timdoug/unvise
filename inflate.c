#include "unvise.h"

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

Buffer inflate_member(Buffer packed, const uint8_t table[256], size_t expected) {
    if (packed.n < 2)
        die("truncated VISE member");

    uint8_t *x = malloc(packed.n);

    if (!x)
        die("out of memory");

    /* Every member byte first passes through VISE's substitution table. */
    for (size_t i = 0; i < packed.n; i++)
        x[i] = table[packed.p[i]];

    if (packed.n >= 2) {
        unsigned word = ((unsigned)x[0] << 8) | x[1];

        /*
         * The VISE 7.3 Dcmp 1005 decoder consumes host-supplied 16-bit words.
         * One host path presents those words in byte-reversed order. A stored
         * block makes the representation self-identifying because LEN and
         * NLEN cease to be complements until each word is swapped.
         */
        if (((word >> 1) & 3) == 0 && packed.n >= 6) {
            unsigned length = ((unsigned)x[2] << 8) | x[3];
            unsigned inverse = ((unsigned)x[4] << 8) | x[5];

            if (length != ((~inverse) & 0xffffu))
                for (size_t i = 0; i + 1 < packed.n; i += 2) {
                    uint8_t byte = x[i];

                    x[i] = x[i + 1];
                    x[i + 1] = byte;
                }
        }
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

Buffer inflate_catalog(Buffer data, size_t catalog) {
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
