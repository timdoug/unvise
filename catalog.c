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

static char *record_name(Buffer data, Record *r, CatalogLayout layout, bool raw_names) {
    if (layout == CATALOG_LITE &&
        (!strcmp(r->tag, "FVCT") || !strcmp(r->tag, "DVCT"))) {
        size_t trailer = !strcmp(r->tag, "FVCT") ? 2 : 8;

        for (size_t p = r->off + 4; p + trailer <= r->end; p++) {
            size_t n = data.p[p];

            if (p + trailer + n != r->end || (!strcmp(r->tag, "FVCT") && data.p[p + 1] != 0x2c))
                continue;

            return convert_name(data.p + p + trailer, n, raw_names);
        }

        return NULL;
    }

    static const uint8_t file_offsets[] = {0x7c, 0x8c, 0xba, 0xbe, 0xc6};
    static const uint8_t directory_offsets[] = {0x58, 0x68, 0x94, 0x98, 0xa0};
    const uint8_t *offsets;
    size_t count;

    if (!strcmp(r->tag, "FVCT")) {
        offsets = file_offsets;
        count = sizeof(file_offsets);
    } else if (!strcmp(r->tag, "DVCT")) {
        offsets = directory_offsets;
        count = sizeof(directory_offsets);
    } else
        return NULL;

    /*
     * The fixed record body grew in several independently selected steps.
     * Names always occupy the complete trailing field. Trying the observed
     * field boundaries in order identifies the actual record shape locally;
     * binary metadata before the name fails valid_name().
     */
    for (size_t i = 0; i < count; i++) {
        size_t start = r->off + offsets[i];

        if (valid_name(data, start, r->end))
            return convert_name(data.p + start, r->end - start, raw_names);
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

static Record *scan_catalog(Buffer data, size_t offset, size_t *count) {
    /*
     * No authoritative record-length field has been identified. Boundaries
     * are inferred by scanning for the four observed record tags.
     */
    size_t capacity = 32, n = 0;
    Record *records = calloc(capacity, sizeof(*records));

    if (!records)
        die("out of memory");

    for (size_t position = offset; position + 4 <= data.n;) {
        while (position + 4 <= data.n && !is_tag(data.p + position))
            position++;
        if (position + 4 > data.n)
            break;

        if (n == capacity) {
            capacity *= 2;
            records = realloc(records, capacity * sizeof(*records));
            if (!records)
                die("out of memory");
            memset(records + n, 0, (capacity - n) * sizeof(*records));
        }

        records[n].off = position;
        memcpy(records[n].tag, data.p + position, 4);
        records[n].tag[4] = 0;
        n++;
        position += 4;
    }

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
        record->name = record_name(data, record, layout, raw_names);

        if (!strcmp(record->tag, "DVCT") && record->end - record->off >= 0x24) {
            record->dir_id = be32(data, record->off + 0x1c);
            record->parent = be32(data, record->off + 0x20);
        } else if (!strcmp(record->tag, "FVCT"))
            decode_file_record(data, record, layout);
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

Record *catalog(Buffer data, size_t offset, CatalogLayout layout, bool raw_names, size_t *count) {
    Record *records = scan_catalog(data, offset, count);

    decode_records(data, records, *count, layout, raw_names);
    if (layout == CATALOG_LITE)
        repair_lite_parents(records, *count);

    return records;
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

char *output_path(const Options *o, Record *all, size_t count, size_t index,
                         const char *fork) {
    Record *r = &all[index];
    char *name = record_path_name(all, r);
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
        parts[np++] = record_path_name(all, d);
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
