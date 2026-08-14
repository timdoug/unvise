#include "unvise.h"

static size_t padded_128(size_t n) {
    if (n > SIZE_MAX - 255)
        die("MacBinary fork is too large");
    return (n + 127) & ~(size_t)127;
}

static bool is_macbinary(Buffer input) {
    size_t data_size, resource_size, resource_offset;
    unsigned name_size;

    if (input.n < 128 || input.p[0] || input.p[74])
        return false;
    name_size = input.p[1];
    if (!name_size || name_size > 63)
        return false;

    data_size = be32(input, 83);
    resource_size = be32(input, 87);
    resource_offset = 128 + padded_128(data_size);
    return resource_offset >= 128 && span(input, 128, data_size) &&
           span(input, resource_offset, resource_size);
}

static void copy_slice(Buffer input, size_t offset, size_t size, Buffer *output) {
    output->p = malloc(size ? size : 1);
    output->n = size;
    if (!output->p)
        die("out of memory");
    memcpy(output->p, input.p + offset, size);
}

static void decode_macbinary(Buffer input, Buffer *data, Buffer *resource,
                             bool *have_resource) {
    size_t data_size = be32(input, 83);
    size_t resource_size = be32(input, 87);
    size_t resource_offset = 128 + padded_128(data_size);

    copy_slice(input, 128, data_size, data);
    if (resource_size) {
        copy_slice(input, resource_offset, resource_size, resource);
        *have_resource = true;
    } else {
        *resource = (Buffer){0};
        *have_resource = false;
    }
}

static int hqx_value(unsigned char c) {
    static const char alphabet[] =
        "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
    const char *p = strchr(alphabet, c);

    return p ? (int)(p - alphabet) : -1;
}

static size_t binhex_marker(Buffer input) {
    static const char marker[] = "(This file must be converted with BinHex 4.0)";
    size_t limit = input.n < 65536 ? input.n : 65536;

    if (limit >= sizeof(marker) - 1)
        for (size_t i = 0; i <= limit - (sizeof(marker) - 1); i++)
            if (!memcmp(input.p + i, marker, sizeof(marker) - 1))
                return i + sizeof(marker) - 1;
    return SIZE_MAX;
}

static bool is_binhex(Buffer input) {
    return binhex_marker(input) != SIZE_MAX ||
           (input.n > 2 && input.p[0] == ':' && hqx_value(input.p[1]) >= 0);
}

static void append(Buffer *output, size_t *capacity, uint8_t byte) {
    if (output->n == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 4096;

        if (next < *capacity)
            die("BinHex data is too large");
        output->p = realloc(output->p, next);
        if (!output->p)
            die("out of memory");
        *capacity = next;
    }
    output->p[output->n++] = byte;
}

static Buffer decode_hqx_text(Buffer input) {
    Buffer packed = {0}, output = {0};
    size_t packed_capacity = 0, output_capacity = 0;
    size_t start = binhex_marker(input);
    unsigned bits = 0, value = 0;
    bool ended = false;

    if (start == SIZE_MAX)
        start = 0;
    while (start < input.n && input.p[start] != ':')
        start++;
    if (start == input.n)
        die("BinHex input lacks an encoded-data delimiter");

    for (size_t i = start + 1; i < input.n; i++) {
        int digit;

        if (input.p[i] == ':') {
            ended = true;
            break;
        }
        digit = hqx_value(input.p[i]);
        if (digit < 0) {
            if (input.p[i] == ' ' || input.p[i] == '\t' || input.p[i] == '\r' ||
                input.p[i] == '\n')
                continue;
            die("invalid character in BinHex data");
        }
        value = (value << 6) | (unsigned)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            append(&packed, &packed_capacity, (uint8_t)(value >> bits));
            value &= bits ? (1u << bits) - 1 : 0;
        }
    }
    if (!ended)
        die("truncated BinHex input");

    for (size_t i = 0; i < packed.n; i++) {
        uint8_t byte = packed.p[i];

        if (byte != 0x90) {
            append(&output, &output_capacity, byte);
            continue;
        }
        if (++i == packed.n)
            die("truncated BinHex run");
        byte = packed.p[i];
        if (!byte) {
            append(&output, &output_capacity, 0x90);
            continue;
        }
        if (!output.n)
            die("invalid BinHex run");
        while (--byte)
            append(&output, &output_capacity, output.p[output.n - 1]);
    }

    free(packed.p);
    return output;
}

static uint16_t binhex_crc(const uint8_t *p, size_t n) {
    uint16_t crc = 0;

    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (unsigned i = 0; i < 8; i++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static void check_binhex_crc(Buffer decoded, size_t start, size_t size, const char *part) {
    uint16_t expected;

    if (!span(decoded, start, size) || !span(decoded, start + size, 2))
        die("truncated BinHex file");
    expected = be16(decoded, start + size);
    if (binhex_crc(decoded.p + start, size) != expected) {
        fprintf(stderr, "unvise: BinHex %s CRC mismatch\n", part);
        exit(1);
    }
}

static void decode_binhex(Buffer input, Buffer *data, Buffer *resource,
                          bool *have_resource) {
    Buffer decoded = decode_hqx_text(input);
    size_t name_size, header_size, data_size, resource_size, data_offset, resource_offset;

    if (!decoded.n)
        die("empty BinHex data");
    name_size = decoded.p[0];
    if (!name_size || name_size > 63)
        die("invalid BinHex filename length");
    header_size = 1 + name_size + 1 + 4 + 4 + 2 + 4 + 4;
    if (!span(decoded, 0, header_size + 2))
        die("truncated BinHex header");

    data_size = be32(decoded, header_size - 8);
    resource_size = be32(decoded, header_size - 4);
    check_binhex_crc(decoded, 0, header_size, "header");
    data_offset = header_size + 2;
    check_binhex_crc(decoded, data_offset, data_size, "data fork");
    resource_offset = data_offset + data_size + 2;
    check_binhex_crc(decoded, resource_offset, resource_size, "resource fork");

    copy_slice(decoded, data_offset, data_size, data);
    if (resource_size) {
        copy_slice(decoded, resource_offset, resource_size, resource);
        *have_resource = true;
    } else {
        *resource = (Buffer){0};
        *have_resource = false;
    }
    free(decoded.p);
}

void unwrap_transport(Buffer *data, Buffer *resource, bool *have_resource) {
    unsigned depth = 0;

    while (is_macbinary(*data) || is_binhex(*data)) {
        Buffer next_data, next_resource;
        bool next_has_resource;

        if (++depth > 16)
            die("too many nested MacBinary or BinHex layers");
        if (is_macbinary(*data))
            decode_macbinary(*data, &next_data, &next_resource, &next_has_resource);
        else
            decode_binhex(*data, &next_data, &next_resource, &next_has_resource);

        free(data->p);
        if (*have_resource)
            free(resource->p);
        *data = next_data;
        *resource = next_resource;
        *have_resource = next_has_resource;
    }
}
