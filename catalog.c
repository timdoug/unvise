#include "unvise.h"

bool catalog_has_late_payloads(CatalogLayout layout) {
    return layout == CATALOG_LATE;
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

void print_quoted(FILE *f, const char *s) {
    fputc('"', f);

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c < 0x20 || c == 0x7f)
            fprintf(f, "\\x%02X", c);
        else if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else
            fputc(c, f);
    }

    fputc('"', f);
}

CatalogLayout catalog_uncompressed_layout(uint8_t generation, uint8_t revision) {
    /*
     * The 68K loaders dispatch on the low byte of the SVCT format field.
     * Revision 0 is Lite. Generations 0 and 2 use short bodies for revisions
     * 1 and 2; generation 3 uses compact bodies for revisions 1--4. Revisions
     * 5--9 use normal bodies, and revisions 10--11 use wide bodies.
     */
    if (revision == 0)
        return CATALOG_LITE;
    if (generation < 3 && revision >= 1 && revision <= 2)
        return CATALOG_SHORT;
    if (revision >= 1 && revision <= 4)
        return CATALOG_COMPACT;
    if (revision >= 10 && revision < 12)
        return CATALOG_WIDE;
    if (revision >= 5 && revision < 10)
        return CATALOG_NORMAL;
    die("unsupported uncompressed catalog revision");
    return CATALOG_NORMAL;
}

CatalogLayout catalog_packed_layout(uint8_t revision) {
    /*
     * The Carbon loaders are separate implementations, but retain the common
     * SVCT revision sequence. The VISE 7.3 PPC loader (revision 11) reads the
     * ordinary 0x94/0xba fixed bodies. The later structures start at revision
     * 12 in VISE 7.4; VISE 8.5 uses revision 14.
     */
    if (revision >= 12)
        return CATALOG_LATE;
    if (revision >= 5)
        return CATALOG_WIDE;
    die("unsupported compressed catalog revision");
    return CATALOG_WIDE;
}

static bool late_next_tag(Buffer data, size_t offset, bool last) {
    if (!span(data, offset, 4))
        return false;
    if (last)
        return !memcmp(data.p + offset, "PACK", 4);
    return !memcmp(data.p + offset, "DVCT", 4) || !memcmp(data.p + offset, "FVCT", 4);
}

static size_t late_file_size(Buffer data, size_t offset) {
    const size_t fixed = 0xc6;

    if (!span(data, offset, fixed))
        die("truncated late FVCT record");

    size_t primary = data.p[offset + 0x7a];
    size_t variable = data.p[offset + 0x7b];
    size_t secondary = data.p[offset + 0xb9];
    unsigned type = data.p[offset + 0x75];
    size_t size = fixed + primary;

    /*
     * BitTst(record + 0x0c, 4) guards the action-parameter tail in both
     * recovered VISE 8.5 PPC loaders.  Classic BitTst numbers bits from the
     * high bit of the first byte, hence mask 0x08 here.  The loader consumes
     * the common variable field before its subtype jump table and consumes
     * the secondary name after it.
     */
    bool action_tail = (data.p[offset + 0x0c] & 0x08) != 0;

    if (action_tail)
        size += variable;

    switch (action_tail ? type : UINT_MAX) {
    case 3:
    case 7:
    case 9:
    case 10: {
        size_t length_offset = size;

        if (!span(data, offset + length_offset, 1))
            die("truncated late action string");
        size = length_offset + 1 + data.p[offset + length_offset];
        break;
    }
    case 4:
        /* Some late replace actions append an optional Pascal display name. */
        if (span(data, offset + size, 4) &&
            memcmp(data.p + offset + size, "DVCT", 4) &&
            memcmp(data.p + offset + size, "FVCT", 4) &&
            memcmp(data.p + offset + size, "PACK", 4))
            size += 1 + data.p[offset + size];
        break;
    case 5: {
        /* Alias actions store variable text, a declared message, then a Pascal name. */
        size_t length_offset = size + be16(data, offset + 0x38);

        if (!span(data, offset + length_offset, 1))
            die("truncated late action string");
        size = length_offset + 1 + data.p[offset + length_offset];
        break;
    }
    case 6:
        size += be16(data, offset + 0x2e) + be16(data, offset + 0x32) +
                be16(data, offset + 0x38);
        break;
    case 14:
        size += be16(data, offset + 0x38);
        break;
    case 15:
        size += data.p[offset + 0xc1] + data.p[offset + 0xc5];
        break;
    case 17:
        size += be16(data, offset + 0x38);
        break;
    default:
        break;
    }

    size += secondary;

    if (!span(data, offset, size))
        die("truncated late FVCT variable fields");
    return size;
}

static uint16_t late_directory_fixed(uint8_t revision) {
    /*
     * Recovered VISE 8.0.2 PPC loaders and revision-13 corpus records use
     * 0xa0 bytes; both recovered VISE 8.5 loaders read 0xa4.
     */
    if (revision == 12 || revision == 13)
        return 0xa0;
    if (revision == 14)
        return 0xa4;
    die("unsupported late catalog revision");
    return 0;
}

static size_t late_directory_size(Buffer data, size_t offset, uint16_t fixed) {
    if (!span(data, offset, fixed))
        die("truncated late DVCT record");

    size_t names = (size_t)data.p[offset + 0x4f] + data.p[offset + 0x50];
    return fixed + names;
}

static Record *parse_late_catalog(Buffer data, size_t expected, uint8_t revision, size_t *count) {
    Record *records = calloc(expected + 1, sizeof(*records));
    size_t offset = 0;

    if (!records)
        die("out of memory");
    for (size_t i = 0; i < expected; i++) {
        if (!span(data, offset, 4))
            die("truncated late catalog");

        Record *record = &records[i];
        record->off = offset;
        memcpy(record->tag, data.p + offset, 4);
        record->tag[4] = 0;

        bool last = i + 1 == expected;
        if (!strcmp(record->tag, "FVCT")) {
            record->fixed_size = 0xc6;
            offset += late_file_size(data, offset);
        } else if (!strcmp(record->tag, "DVCT")) {
            record->fixed_size = late_directory_fixed(revision);
            offset += late_directory_size(data, offset, record->fixed_size);
        } else
            die("invalid late catalog record signature");

        if (!late_next_tag(data, offset, last))
            die("invalid late catalog record length");
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
    /* Classic BitTst(..., 4) addresses mask 0x08 in the first stored byte. */
    bool action = (be32(data, offset + 0x0c) & UINT32_C(0x08000000)) != 0;

    if (!action) {
        size += secondary;
        if (!span(data, offset, size))
            die("truncated FVCT variable fields");
        return size;
    }

    switch (type) {
    case 8:
        size += variable;
        break;
    case 1:
        if (fixed >= 0xba)
            size += variable + be16(data, offset + 0x38);
        break;
    case 2:
        /* Move/replace actions append their +0x38-sized message text. */
        if (fixed >= 0xba)
            size += variable + be16(data, offset + 0x38);
        break;
    case 3:
    case 7:
    case 9:
    case 10: {
        size_t length_offset = size + variable;

        if (!span(data, offset + length_offset, 1))
            die("truncated action string");
        size = length_offset + 1 + data.p[offset + length_offset];
        break;
    }
    case 5: {
        /* Alias actions store variable text, a declared message, then a Pascal name. */
        size_t length_offset = size + variable + be16(data, offset + 0x38);

        if (!span(data, offset + length_offset, 1))
            die("truncated action string");
        size = length_offset + 1 + data.p[offset + length_offset];
        break;
    }
    case 4:
        size += variable + be16(data, offset + 0x38);
        break;
    case 6:
        size += variable + be16(data, offset + 0x2e) + be16(data, offset + 0x32) +
                be16(data, offset + 0x38);
        break;
    case 13:
        size += variable + secondary;
        break;
    case 11:
    case 12:
        size += variable;
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

static size_t compact_record_size(Buffer data, size_t offset, bool directory, uint8_t revision,
                                  uint16_t *fixed) {
    size_t base_fixed = directory ? 0x58 : 0x7c;
    size_t body_extension = revision >= 4 ? 0x10 : 0;
    size_t record_fixed = base_fixed + body_extension;
    size_t length_offset = directory ? 0x50 : 0x7a;

    if (!span(data, offset + length_offset, 1))
        die("truncated compact catalog record");
    size_t names = data.p[offset + length_offset];
    size_t size;

    if (directory) {
        size = record_fixed + names;
    } else {
        bool action =
            (be32(data, offset + 0x0c) & UINT32_C(0x08000000)) != 0;

        if (action && (data.p[offset + 0x75] == 4 || data.p[offset + 0x75] == 6)) {
            size_t extra = be16(data, offset + 0x38);

            if (data.p[offset + 0x75] == 6)
                extra += be16(data, offset + 0x2e) + be16(data, offset + 0x32);
            size = record_fixed + names + extra;
        } else {
            size = variable_file_size(data, offset, record_fixed);
        }
    }
    if (!span(data, offset, size))
        die("truncated compact catalog record");
    *fixed = (uint16_t)record_fixed;
    return size;
}

static Record *parse_catalog_records(Buffer data, size_t offset, size_t expected,
                                     CatalogLayout layout, uint8_t revision, size_t *count) {
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

        if (layout == CATALOG_SHORT) {
            if (!directory)
                position += compact_record_size(data, position, false, revision,
                                                &record->fixed_size);
            else {
                size_t fixed = 0x52;

                if (!span(data, position + 0x50, 1))
                    die("truncated short DVCT record");
                record->fixed_size = (uint16_t)fixed;
                position += fixed + data.p[position + 0x50];
            }
        } else if (layout == CATALOG_COMPACT)
            position +=
                compact_record_size(data, position, directory, revision, &record->fixed_size);
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
    record->record_flags = be32(data, record->off + 0x0c);
    record->expanded[0] = be32(data, record->off + 0x48);
    record->packed[1] = be32(data, record->off + 0x4c);
    record->expanded[1] = be32(data, record->off + 0x50);
    record->checksum = be32(data, record->off + 0x54);
    record->segment = be16(data, record->off + 0x62);
    record->payload = be32(data, record->off + 0x64);
    record->payload_mode = be32(data, record->off + 0x60);
    if (record->end - record->off > 0x75)
        record->subtype = data.p[record->off + 0x75];
    /*
     * The VISE 4.5 68K and VISE 8.5 PPC loaders independently copy raw
     * +0x3c/+0x40 to internal +0x42/+0x46.  Their file-info paths use those
     * internal fields as classic creation and modification dates.
     */
    record->created = be32(data, record->off + 0x3c);
    record->modified = be32(data, record->off + 0x40);
    memcpy(record->finder_info, data.p + record->off + 0x2c,
           sizeof(record->finder_info));

    if (layout == CATALOG_LITE) {
        record->file = (record->record_flags & UINT32_C(0x08000000)) == 0;
        record->parent = be32(data, record->off + 0x58);
        return;
    }

    /* The original loaders use bit 4 of the first flag byte for actions. */
    record->file = (record->record_flags & UINT32_C(0x08000000)) == 0;
    record->parent = be32(data, record->off +
                                     (layout == CATALOG_COMPACT || layout == CATALOG_SHORT ?
                                          0x60 : 0x58));
    if (record->file && record->end - record->off >= 0x70) {
        record->fork_offset[0] = be32(data, record->off + 0x68);
        record->fork_offset[1] = be32(data, record->off + 0x6c);
        record->has_fork_offsets = record->fork_offset[0] || record->fork_offset[1];
    }
    if (layout == CATALOG_LATE) {
        record->depth = be16(data, record->off + 0x60);
    }
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
            /* Early creators encode a top-level install folder as its own parent. */
            if (record->parent == record->dir_id)
                record->parent = 0;
            /*
             * The 68K directory loader copies raw +0x04..+0x13 as Finder
             * information and raw +0x14/+0x18 as creation/modification time.
             * The VISE 8 PPC loader performs the same 16-byte metadata copy.
             */
            memcpy(record->finder_info, data.p + record->off + 4,
                   sizeof(record->finder_info));
            record->created = be32(data, record->off + 0x14);
            record->modified = be32(data, record->off + 0x18);
            if (layout == CATALOG_LATE)
                record->depth = be16(data, record->off + 0x48);
        } else if (!strcmp(record->tag, "FVCT"))
            decode_file_record(data, record, layout);
    }

    if (layout == CATALOG_LATE) {
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
                die("invalid late catalog depth");
            record->parent = record->depth ? parents[record->depth - 1] : 0;
            if (!strcmp(record->tag, "DVCT")) {
                record->dir_id = (uint32_t)i + 1;
                parents[record->depth] = record->dir_id;
            }
        }
        free(parents);
    }

    if (layout == CATALOG_WIDE || layout == CATALOG_LATE)
        for (size_t i = 0; i < count; i++) {
            Record *record = &records[i];

            /*
             * The low payload-mode bit selects a complete member. With the
             * bit clear, the record is an updater whose decoder history comes
             * from the installed file; a complete alternative need not be in
             * the catalog. Higher bits vary between loader builds.
             */
            if (record->file && !(record->payload_mode & 1))
                record->base_dependent = true;
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

static void recover_lite_paths(Record *records, size_t count) {
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

        /* A destination used as a DVCT parent denotes the virtual root. */
        if (directory_has_parent(records, count, record->parent)) {
            current_dir = 0;
            continue;
        }

        /*
         * Lite represents a custom folder icon as an FVCT named "Icon\r"
         * with Finder type 'icon' and creator 'MACS'. Its +0x58 value is a
         * runtime destination object rather than a DVCT ID. The operation
         * applies to the directory immediately preceding it in preorder.
         */
        if (current_dir && record->name && !strcmp(record->name, "Icon\r") &&
            !memcmp(record->finder_info, "iconMACS", 8)) {
            record->parent = current_dir;
            continue;
        }

        /*
         * +0x58 is also used for runtime destination objects (system folders,
         * selected install locations, and similar aliases). It is not a
         * catalog parent ID in that form. Lite's catalog order keeps the file
         * under the current DVCT; the recovered loader builds the same
         * hierarchy as an internal preorder/depth array.
         */
        record->parent = current_dir;
    }
}

static char *safe_name(const char *name) {
    size_t n = strlen(name);
    char *s = malloc(n + 32), *q = s;
    if (!s)
        die("out of memory");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0)
            continue;
        *q++ = (c == '/' ? ':' : (char)c);
    }
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
        return safe_name(record->name);

    char fallback[32];
    snprintf(fallback, sizeof(fallback), "unnamed-%04zu", (size_t)(record - all));
    return safe_name(fallback);
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
    bool *visited = calloc(count, sizeof(bool));
    size_t np = 0;
    uint32_t parent = r->parent;
    if (!parts || !visited)
        die("out of memory");
    for (size_t guard = 0; guard < count; guard++) {
        Record *d = dir_by_id(all, count, parent);
        if (!d)
            break;
        size_t directory = (size_t)(d - all);

        if (visited[directory])
            die("directory parent cycle");
        visited[directory] = true;
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
    free(visited);
    free(parts);
    free(name);
    return path;
}

static bool record_outputs_fork(const Record *record) {
    return !strcmp(record->tag, "FVCT") && record->file &&
           (record->expanded[0] || record->expanded[1]) &&
           (record->packed[0] || record->packed[1]);
}

static bool record_is_file(const Record *record) {
    return !strcmp(record->tag, "FVCT") && record->file;
}

static bool path_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return false;
    }
    return *a == *b;
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
        all[i].output_group = SIZE_MAX;
    }

    /*
     * Preserve the ordinary pathname for the first output-bearing record.
     * Later records targeting that same catalog path are conditional
     * alternatives and receive their stable record number.
     */
    for (size_t i = 0; i < count; i++) {
        if (!record_outputs_fork(&all[i]))
            continue;
        for (size_t j = 0; j < i; j++)
            if (record_outputs_fork(&all[j]) && path_equal(all[i].path, all[j].path)) {
                all[j].output_group = j;
                all[i].output_group = j;
                append_record_suffix(&all[i], i);
                break;
            }
    }

    /*
     * Installation conditions can also select either a file or a directory
     * at one logical path. Keep the directory hierarchy at its ordinary name
     * and suffix the file alternative, which cannot coexist with it on HFS,
     * APFS, or a POSIX filesystem.
     */
    for (size_t i = 0; i < count; i++) {
        if (!record_is_file(&all[i]))
            continue;
        for (size_t j = 0; j < count; j++)
            if (!strcmp(all[j].tag, "DVCT") && path_equal(all[i].path, all[j].path)) {
                append_record_suffix(&all[i], i);
                break;
            }
    }
}

Record *catalog(Buffer data, size_t offset, size_t expected, CatalogLayout layout, bool raw_names,
                uint8_t revision, size_t *count) {
    Record *records = layout == CATALOG_LATE ? parse_late_catalog(data, expected, revision, count) :
                                               parse_catalog_records(data, offset, expected, layout,
                                                                     revision, count);

    decode_records(data, records, *count, layout, raw_names);
    if (layout == CATALOG_LITE)
        recover_lite_paths(records, *count);
    plan_output_paths(records, *count);

    return records;
}

char *output_path(const Options *o, Record *all, size_t index, const char *fork) {
    const char *relative = all[index].path;
    size_t cap = strlen(o->out) + strlen(relative) + 128;
    char *path = malloc(cap);

    if (!path)
        die("out of memory");

    strcpy(path, o->out);
    strcat(path, "/");
    strcat(path, relative);
    if (!o->native && !o->appledouble) {
        strcat(path, ".");
        strcat(path, fork);
    }
    make_parent_dir(path);
    return path;
}
