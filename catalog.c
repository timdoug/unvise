#include "unvise.h"

bool catalog_is_packed(CatalogLayout layout) {
    return layout == CATALOG_COMPRESSED || layout == CATALOG_VISE8;
}

bool catalog_has_vise8_payloads(CatalogLayout layout) {
    return layout == CATALOG_VISE8;
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

static bool valid_name(Buffer data, size_t start, size_t end) {
    if (start >= end)
        return false;

    for (size_t i = start; i < end; i++)
        if ((data.p[i] < 0x20 && data.p[i] != '\r') || data.p[i] == 0x7f)
            return false;

    return true;
}

static char *record_name(Buffer data, Record *r, bool raw_names) {
    if (r->fixed_size) {
        size_t length_offset;

        if (!strcmp(r->tag, "FVCT")) {
            length_offset = 0x7a;
        } else if (!strcmp(r->tag, "DVCT")) {
            length_offset = 0x50;
        } else
            return NULL;

        if (!span(data, r->off + length_offset, 1))
            return NULL;
        size_t length = data.p[r->off + length_offset];
        if (!length || r->fixed_size > r->end - r->off ||
            length > r->end - r->off - r->fixed_size)
            return NULL;
        return convert_name(data.p + r->off + r->fixed_size, length, raw_names);
    }

    return NULL;
}

void print_quoted(const char *s, bool raw) {
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

CatalogLayout catalog_direct_layout(Buffer data, size_t offset) {
    /*
     * A literal substitution table is used by both Lite and some full VISE
     * self-installers. Full compact records expose a complete trailing name at
     * one of their two observed boundaries; Lite records instead use a Pascal
     * name and trailer.
     */
    for (size_t position = offset; position + 4 <= data.n;) {
        while (position + 4 <= data.n && !is_tag(data.p + position))
            position++;
        if (position + 4 > data.n)
            break;

        size_t end = position + 4;
        while (end + 4 <= data.n && !is_tag(data.p + end))
            end++;

        if (!memcmp(data.p + position, "DVCT", 4) &&
            (valid_name(data, position + 0x58, end) ||
             valid_name(data, position + 0x68, end)))
            return CATALOG_COMPACT;
        if (!memcmp(data.p + position, "FVCT", 4) &&
            (valid_name(data, position + 0x7c, end) ||
             valid_name(data, position + 0x8c, end)))
            return CATALOG_COMPACT;

        position = end;
    }

    return CATALOG_LITE;
}

CatalogLayout catalog_pef_layout(Buffer data) {
    /*
     * Carbon-era VISE 7 and VISE 8 both keep the substitution table in the
     * PEF application rather than DATA 0. At the ordinary compressed-catalog
     * name offset VISE 8 has binary metadata, while VISE 7 has a complete
     * printable MacRoman name.
     */
    for (size_t offset = 0; offset + 4 <= data.n;) {
        while (offset + 4 <= data.n && !is_tag(data.p + offset))
            offset++;
        if (offset + 4 > data.n)
            break;

        size_t end = offset + 4;
        while (end + 4 <= data.n && !is_tag(data.p + end))
            end++;
        if (!memcmp(data.p + offset, "DVCT", 4) &&
            valid_name(data, offset + 0x98, end))
            return CATALOG_COMPRESSED;
        if (!memcmp(data.p + offset, "FVCT", 4) &&
            valid_name(data, offset + 0xbe, end))
            return CATALOG_COMPRESSED;

        offset = end;
    }

    return CATALOG_VISE8;
}

static bool vise8_next_tag(Buffer data, size_t offset, bool last) {
    if (!span(data, offset, 4))
        return false;
    if (last)
        return !memcmp(data.p + offset, "PACK", 4);
    return !memcmp(data.p + offset, "DVCT", 4) || !memcmp(data.p + offset, "FVCT", 4);
}

static size_t vise8_file_size(Buffer data, size_t offset) {
    const size_t fixed = 0xc6;

    if (!span(data, offset, fixed))
        die("truncated VISE 8 FVCT record");

    size_t primary = data.p[offset + 0x7a];
    size_t variable = data.p[offset + 0x7b];
    size_t secondary = data.p[offset + 0xb9];
    unsigned type = data.p[offset + 0x75];
    size_t size = fixed + primary;
    uint32_t record_type = be32(data, offset + 4);
    bool parameter = (be32(data, offset + 0x44) == UINT32_C(0x00010001) ||
                      be32(data, offset + 0x44) == UINT32_C(0x00020001) ||
                      be32(data, offset + 0x44) == UINT32_C(0x00040001)) &&
                     be32(data, offset + 0x4c) == UINT32_C(0x00010001);
    bool file = !parameter &&
                (record_type == 0 || record_type >= UINT32_C(0x00010000)) &&
                (record_type >> 24) != 3;

    if (file) {
        size += secondary;
        if (!span(data, offset, size))
            die("truncated VISE 8 FVCT variable fields");
        return size;
    }

    switch (type) {
    case 3:
    case 5:
    case 9:
    case 10: {
        size_t length_offset = size + variable;

        if (!span(data, offset + length_offset, 1))
            die("truncated VISE 8 action string");
        size = length_offset + 1 + data.p[offset + length_offset];
        break;
    }
    case 6:
        size += variable + be16(data, offset + 0x2e) + be16(data, offset + 0x32) +
                be16(data, offset + 0x38);
        break;
    case 13:
        size += variable + secondary;
        break;
    default:
        size += secondary;
        break;
    }

    if (!span(data, offset, size))
        die("truncated VISE 8 FVCT variable fields");
    return size;
}

static size_t vise8_directory_size(Buffer data, size_t offset, bool last, uint16_t *fixed) {
    if (!span(data, offset, 0xa4))
        die("truncated VISE 8 DVCT record");

    size_t names = (size_t)data.p[offset + 0x4f] + data.p[offset + 0x50];
    size_t old_size = 0xa0 + names;
    size_t new_size = 0xa4 + names;
    bool old_valid = vise8_next_tag(data, offset + old_size, last);
    bool new_valid = vise8_next_tag(data, offset + new_size, last);

    if (old_valid == new_valid)
        die("ambiguous or invalid VISE 8 DVCT length");
    *fixed = old_valid ? 0xa0 : 0xa4;
    return old_valid ? old_size : new_size;
}

static Record *parse_vise8_catalog(Buffer data, size_t expected, size_t *count) {
    Record *records = calloc(expected + 1, sizeof(*records));
    size_t offset = 0;

    if (!records)
        die("out of memory");
    for (size_t i = 0; i < expected; i++) {
        if (!span(data, offset, 4))
            die("truncated VISE 8 catalog");

        Record *record = &records[i];
        record->off = offset;
        memcpy(record->tag, data.p + offset, 4);
        record->tag[4] = 0;

        bool last = i + 1 == expected;
        if (!strcmp(record->tag, "FVCT")) {
            record->fixed_size = 0xc6;
            offset += vise8_file_size(data, offset);
        } else if (!strcmp(record->tag, "DVCT"))
            offset += vise8_directory_size(data, offset, last, &record->fixed_size);
        else
            die("invalid VISE 8 catalog record signature");

        if (!vise8_next_tag(data, offset, last))
            die("invalid VISE 8 catalog record length");
    }

    records[expected].off = offset;
    memcpy(records[expected].tag, "PACK", 5);
    *count = expected + 1;
    return records;
}

static size_t variable_file_size(Buffer data, size_t offset, size_t fixed) {
    if (!span(data, offset, fixed))
        die("truncated FVCT record");

    size_t primary = data.p[offset + 0x7a];
    size_t variable = fixed > 0x7b ? data.p[offset + 0x7b] : 0;
    size_t secondary = fixed > 0xb9 ? data.p[offset + 0xb9] : 0;
    unsigned type = fixed > 0x75 ? data.p[offset + 0x75] : 0;
    size_t size = fixed + primary;
    uint32_t record_type = be32(data, offset + 4);
    bool parameter = (be32(data, offset + 0x44) == UINT32_C(0x00010001) ||
                      be32(data, offset + 0x44) == UINT32_C(0x00020001) ||
                      be32(data, offset + 0x44) == UINT32_C(0x00040001)) &&
                     be32(data, offset + 0x4c) == UINT32_C(0x00010001);
    bool file = !parameter &&
                (record_type == 0 || record_type >= UINT32_C(0x00010000)) &&
                (record_type >> 24) != 3;

    if (file) {
        size += secondary;
        if (!span(data, offset, size))
            die("truncated FVCT variable fields");
        return size;
    }

    switch (type) {
    case 1:
    case 4:
    case 8:
        size += variable;
        break;
    case 2:
        if ((record_type >> 24) != 3)
            size += variable;
        break;
    case 3:
    case 5:
    case 9:
    case 10: {
        size_t length_offset = size + variable;

        if (!span(data, offset + length_offset, 1))
            die("truncated action string");
        size = length_offset + 1 + data.p[offset + length_offset];
        break;
    }
    case 6:
        size += variable + be16(data, offset + 0x2e) + be16(data, offset + 0x32) +
                be16(data, offset + 0x38);
        break;
    case 13:
        size += variable + secondary;
        break;
    default:
        size += secondary;
        break;
    }

    if (!span(data, offset, size))
        die("truncated FVCT variable fields");
    return size;
}

static bool semantic_tag(Buffer data, size_t offset) {
    return span(data, offset, 4) &&
           (!memcmp(data.p + offset, "DVCT", 4) || !memcmp(data.p + offset, "FVCT", 4));
}

static size_t compact_record_size(Buffer data, size_t offset, bool directory, uint16_t *fixed) {
    size_t short_fixed = directory ? 0x58 : 0x7c;
    size_t long_fixed = short_fixed + 0x10;
    size_t length_offset = directory ? 0x50 : 0x7a;

    if (!span(data, offset + length_offset, 1))
        die("truncated compact catalog record");
    size_t names = data.p[offset + length_offset];
    size_t short_size;
    size_t long_size;

    if (directory) {
        short_size = short_fixed + names;
        long_size = long_fixed + names;
    } else {
        uint32_t record_type = be32(data, offset + 4);
        bool parameter = (be32(data, offset + 0x44) == UINT32_C(0x00010001) ||
                          be32(data, offset + 0x44) == UINT32_C(0x00020001) ||
                          be32(data, offset + 0x44) == UINT32_C(0x00040001)) &&
                         be32(data, offset + 0x4c) == UINT32_C(0x00010001);
        bool file = !parameter &&
                    (record_type == 0 || record_type >= UINT32_C(0x00010000)) &&
                    (record_type >> 24) != 3;

        if (!file && (data.p[offset + 0x75] == 4 || data.p[offset + 0x75] == 6)) {
            size_t extra = be16(data, offset + 0x38);

            if (data.p[offset + 0x75] == 6)
                extra += be16(data, offset + 0x2e) + be16(data, offset + 0x32);
            short_size = short_fixed + names + extra;
            long_size = long_fixed + names + extra;
        } else {
            short_size = variable_file_size(data, offset, short_fixed);
            long_size = variable_file_size(data, offset, long_fixed);
        }
    }
    bool short_valid = semantic_tag(data, offset + short_size) ||
                       (span(data, offset + short_size, 4) &&
                        !memcmp(data.p + offset + short_size, "PACK", 4));
    bool long_valid = semantic_tag(data, offset + long_size) ||
                      (span(data, offset + long_size, 4) &&
                       !memcmp(data.p + offset + long_size, "PACK", 4));

    if (short_valid == long_valid)
        die("ambiguous or invalid compact catalog record length");
    *fixed = short_valid ? (uint16_t)short_fixed : (uint16_t)long_fixed;
    return short_valid ? short_size : long_size;
}

static Record *parse_catalog_records(Buffer data, size_t offset, size_t expected,
                                     CatalogLayout layout, size_t *count) {
    size_t capacity = expected + 3;
    Record *records = calloc(capacity, sizeof(*records));
    size_t n = 0, position = offset;

    if (!records)
        die("out of memory");

    if (span(data, position, 4) && !memcmp(data.p + position, "CVCT", 4)) {
        records[n].off = position;
        memcpy(records[n].tag, "CVCT", 5);
        n++;
        position += 0x14;

        if (span(data, position, 4) && !memcmp(data.p + position, "PACK", 4)) {
            if (!span(data, position, 0x50) || !semantic_tag(data, position + 0x50))
                die("invalid leading PACK record");
            records[n].off = position;
            memcpy(records[n].tag, "PACK", 5);
            n++;
            position += 0x50;
        }
    }

    for (size_t i = 0; i < expected; i++) {
        if (!semantic_tag(data, position))
            die("invalid catalog record signature");

        Record *record = &records[n++];
        bool directory = !memcmp(data.p + position, "DVCT", 4);
        record->off = position;
        memcpy(record->tag, data.p + position, 4);
        record->tag[4] = 0;

        if (layout == CATALOG_COMPACT)
            position += compact_record_size(data, position, directory, &record->fixed_size);
        else if (directory) {
            size_t fixed = layout == CATALOG_LITE ? 0x58 :
                           layout == CATALOG_NORMAL ? 0x94 : 0x98;

            if (!span(data, position + 0x50, 1))
                die("truncated DVCT record");
            record->fixed_size = (uint16_t)fixed;
            position += fixed + data.p[position + 0x50];
        } else {
            size_t fixed = layout == CATALOG_LITE ? 0x7c :
                           layout == CATALOG_NORMAL ? 0xba : 0xbe;
            record->fixed_size = (uint16_t)fixed;
            position += variable_file_size(data, position, fixed);
        }

        if (i + 1 < expected && !semantic_tag(data, position))
            die("invalid catalog record length");
    }

    if (!span(data, position, 4) || memcmp(data.p + position, "PACK", 4))
        die("catalog lacks trailing PACK record");
    records[n].off = position;
    memcpy(records[n].tag, "PACK", 5);
    n++;
    *count = n;
    return records;
}

static void decode_file_record(Buffer data, Record *record, CatalogLayout layout) {
    if (record->end - record->off < 0x68)
        return;

    record->packed[0] = be32(data, record->off + 0x44);
    record->expanded[0] = be32(data, record->off + 0x48);
    record->packed[1] = be32(data, record->off + 0x4c);
    record->expanded[1] = be32(data, record->off + 0x50);
    record->payload = be32(data, record->off + 0x64);

    /*
     * Search/delete actions reuse fork fields for parameters. This sentinel
     * distinguishes them from lengths in both compact and later layouts.
     */
    bool parameter_record = (record->packed[0] == UINT32_C(0x00010001) ||
                             record->packed[0] == UINT32_C(0x00020001) ||
                             record->packed[0] == UINT32_C(0x00040001)) &&
                            record->packed[1] == UINT32_C(0x00010001);

    if (layout == CATALOG_LITE) {
        record->file = !parameter_record &&
                       (record->packed[0] || record->packed[1] || record->expanded[0] ||
                        record->expanded[1]);
        record->parent = be32(data, record->off + 0x58);
        return;
    }

    uint32_t type = be32(data, record->off + 4);

    record->file = !parameter_record && (type == 0 || type >= UINT32_C(0x00010000)) &&
                   (type >> 24) != 3;
    record->parent = be32(data, record->off + (layout == CATALOG_COMPACT ? 0x60 : 0x58));
    if (layout == CATALOG_VISE8)
        record->depth = be16(data, record->off + 0x60);

    if (record->end - record->off >= 0x70 &&
        !memcmp(data.p + record->off + 0x2c, "issp", 4) && be32(data, record->off + 0x68) &&
        be32(data, record->off + 0x6c))
        record->gap = be32(data, record->off + 0x68);
}

static void decode_records(Buffer data, Record *records, size_t count, CatalogLayout layout,
                           bool raw_names) {
    for (size_t i = 0; i < count; i++) {
        Record *record = &records[i];

        record->end = i + 1 < count ? records[i + 1].off : data.n;
        record->name = record_name(data, record, raw_names);

        if (!strcmp(record->tag, "DVCT") && record->end - record->off >= 0x24) {
            record->dir_id = be32(data, record->off + 0x1c);
            record->parent = be32(data, record->off + 0x20);
            if (layout == CATALOG_VISE8)
                record->depth = be16(data, record->off + 0x48);
        } else if (!strcmp(record->tag, "FVCT"))
            decode_file_record(data, record, layout);
    }

    if (layout == CATALOG_VISE8) {
        /*
         * The VISE 8 PPC loaders read the nesting level from +0x48 in DVCT
         * and +0x60 in FVCT. Records are stored in preorder. VISE 8.5 uses
         * the older FVCT parent-ID field for unrelated metadata, so the
         * explicit level is the authoritative hierarchy representation.
         */
        uint32_t *parents = calloc(count + 1, sizeof(*parents));

        if (!parents)
            die("out of memory");
        for (size_t i = 0; i < count; i++) {
            Record *record = &records[i];

            if (strcmp(record->tag, "DVCT") && strcmp(record->tag, "FVCT"))
                continue;
            if (record->depth > count || (record->depth && !parents[record->depth - 1]))
                die("invalid VISE 8 catalog depth");
            record->parent = record->depth ? parents[record->depth - 1] : 0;
            if (!strcmp(record->tag, "DVCT")) {
                record->dir_id = (uint32_t)i + 1;
                parents[record->depth] = record->dir_id;
            }
        }
        free(parents);
    }
}

static bool directory_has_id(const Record *records, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(records[i].tag, "DVCT") && records[i].dir_id == id)
            return true;
    return false;
}

static bool directory_has_parent(const Record *records, size_t count, uint32_t parent) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(records[i].tag, "DVCT") && records[i].parent == parent)
            return true;
    return false;
}

static void repair_lite_parents(Record *records, size_t count) {
    uint32_t current_dir = 0;

    for (size_t i = 0; i < count; i++) {
        Record *record = &records[i];

        if (!strcmp(record->tag, "DVCT")) {
            current_dir = record->dir_id;
            continue;
        }
        if (strcmp(record->tag, "FVCT") || !record->file ||
            directory_has_id(records, count, record->parent))
            continue;

        /*
         * Lite 3.6 may store an install-location token instead of a DVCT ID.
         * Catalog order retains the containing directory association.
         */
        if (directory_has_parent(records, count, record->parent))
            current_dir = 0;
        else if (current_dir)
            record->parent = current_dir;
    }
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

static char *record_path_name(const Record *all, const Record *record) {
    if (record->name)
        return safe_name(record->name, false);

    char fallback[32];
    snprintf(fallback, sizeof(fallback), "unnamed-%04zu", (size_t)(record - all));
    return safe_name(fallback, false);
}

static Record *dir_by_id(Record *r, size_t n, uint32_t id) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(r[i].tag, "DVCT") && r[i].dir_id == id)
            return &r[i];
    return NULL;
}

static char *record_relative_path(Record *all, size_t count, size_t index) {
    Record *r = &all[index];
    char *name = record_path_name(all, r);
    size_t cap = strlen(name) + 128;
    char **parts = calloc(count, sizeof(char *));
    size_t np = 0;
    uint32_t parent = r->parent;
    if (!parts)
        die("out of memory");
    for (size_t guard = 0; guard < count; guard++) {
        Record *d = dir_by_id(all, count, parent);
        if (!d)
            break;
        parts[np++] = record_path_name(all, d);
        parent = d->parent;
    }
    for (size_t i = 0; i < np; i++)
        cap += strlen(parts[i]) + 1;
    char *path = malloc(cap);
    if (!path)
        die("out of memory");
    *path = 0;
    for (size_t i = np; i; i--) {
        if (*path)
            strcat(path, "/");
        strcat(path, parts[i - 1]);
        free(parts[i - 1]);
    }
    if (*path)
        strcat(path, "/");
    strcat(path, name);
    free(parts);
    free(name);
    return path;
}

static bool record_outputs_fork(const Record *record) {
    return !strcmp(record->tag, "FVCT") && record->file &&
           (record->expanded[0] || record->expanded[1]) &&
           (record->packed[0] || record->packed[1]);
}

static void append_record_suffix(Record *record, size_t index) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "~%04zu", index);
    size_t n = strlen(record->path);
    char *path = realloc(record->path, n + strlen(suffix) + 1);

    if (!path)
        die("out of memory");
    record->path = path;
    strcpy(record->path + n, suffix);
}

static void plan_output_paths(Record *all, size_t count) {
    for (size_t i = 0; i < count; i++) {
        all[i].path = record_relative_path(all, count, i);
        if (record_outputs_fork(&all[i]))
            append_record_suffix(&all[i], i);
    }
}

Record *catalog(Buffer data, size_t offset, size_t expected, CatalogLayout layout, bool raw_names,
                size_t *count) {
    Record *records = layout == CATALOG_VISE8 ? parse_vise8_catalog(data, expected, count) :
                                               parse_catalog_records(data, offset, expected, layout,
                                                                     count);

    decode_records(data, records, *count, layout, raw_names);
    if (layout == CATALOG_LITE)
        repair_lite_parents(records, *count);
    plan_output_paths(records, *count);

    return records;
}

char *output_path(const Options *o, Record *all, size_t index, const char *fork,
                  const char *variant) {
    const char *relative = all[index].path;
    size_t cap = strlen(o->out) + strlen(relative) + 128;
    char *path = malloc(cap);

    if (!path)
        die("out of memory");

    strcpy(path, o->out);
    strcat(path, "/");
    strcat(path, relative);
    if (variant) {
        strcat(path, "-");
        strcat(path, variant);
    }
    if (!o->native && !o->appledouble) {
        strcat(path, ".");
        strcat(path, fork);
    }
    make_parent_dir(path);
    return path;
}
