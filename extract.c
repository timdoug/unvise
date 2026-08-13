#include "unvise.h"

typedef struct {
    const Options *options;
    Buffer data;
    uint32_t catalog_offset;
    CatalogLayout layout;
    Record *records;
    size_t count;
    uint8_t table[256];
} Extraction;

static bool record_has_payload(const Record *record) {
    return !strcmp(record->tag, "FVCT") && record->file &&
           (record->expanded[0] || record->expanded[1]) &&
           (record->packed[0] || record->packed[1]);
}

static bool same_payload(const Record *record, uint32_t payload) {
    return record_has_payload(record) && record->payload == payload;
}

static void print_record(const Record *record, size_t index, bool raw_names) {
    printf("%04zu 0x%08zX %-4s size=0x%zX", index, record->off, record->tag,
           record->end - record->off);

    if (!strcmp(record->tag, "FVCT") && record->file)
        printf(" payload=0x%X data=0x%X->0x%X rsrc=0x%X->0x%X", record->payload,
               record->packed[0], record->expanded[0], record->packed[1], record->expanded[1]);
    else if (!strcmp(record->tag, "FVCT"))
        fputs(" action", stdout);

    if (record->name) {
        fputs(" name=", stdout);
        print_quoted(record->name, raw_names);
    }

    putchar('\n');
}

static void check_payload(const Extraction *x, size_t offset, size_t packed_size) {
    if (catalog_is_packed(x->layout) && offset >= x->catalog_offset)
        die("installer uses an external web payload archive; the stub alone cannot be extracted");
    if (offset > x->catalog_offset || packed_size > x->catalog_offset - offset)
        die("payload overlaps catalog");
}

static void write_fork_variant(const Extraction *x, size_t index, int fork, const uint8_t *data,
                               size_t size, const char *variant) {
    const char *kind = fork ? "rsrc" : "data";
    char *path = output_path(x->options, x->records, index, kind, variant);

    write_output(x->options, path, kind, data, size);
    free(path);
}

static void write_fork(const Extraction *x, size_t index, int fork, const uint8_t *data,
                       size_t size) {
    write_fork_variant(x, index, fork, data, size, NULL);
}

static void extract_version_source(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];
    size_t total = record->packed[1];
    size_t resource_start = (size_t)record->expanded[0] + record->gap;

    if (resource_start > total || record->expanded[1] > total - resource_start)
        die("version-source payload layout overflow");

    check_payload(x, record->payload, record->packed[0]);

    Buffer expanded =
        inflate_member(slice(x->data, record->payload, record->packed[0]), x->table, total);

    if (record->expanded[0])
        write_fork_variant(x, index, 0, expanded.p, record->expanded[0], "version");
    if (record->expanded[1])
        write_fork_variant(x, index, 1, expanded.p + resource_start, record->expanded[1],
                           "version");

    free(expanded.p);
}

static size_t shared_records(const Extraction *x, uint32_t payload, size_t *first) {
    size_t count = 0;

    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], payload)) {
            if (!count)
                *first = i;
            count++;
        }

    return count;
}

static size_t shared_expanded_size(const Extraction *x, uint32_t payload) {
    size_t size = 0;

    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], payload))
            for (int fork = 0; fork < 2; fork++) {
                if (x->records[i].expanded[fork] > SIZE_MAX - size)
                    die("shared payload is too large");
                size += x->records[i].expanded[fork];
            }

    return size;
}

static size_t shared_packed_size(const Extraction *x, const Record *record,
                                 size_t *expanded_size) {
    if (record->packed[0] && record->packed[1] == *expanded_size)
        return record->packed[0];
    if (!!record->packed[0] != !!record->packed[1])
        return record->packed[0] ? record->packed[0] : record->packed[1];
    if (catalog_has_vise8_payloads(x->layout)) {
        *expanded_size = record->packed[1];
        return record->packed[0];
    }

    fprintf(stderr,
            "unvise: unknown shared payload layout at 0x%X "
            "(0x%X, 0x%X, expanded 0x%zx)\n",
            record->payload, record->packed[0], record->packed[1], *expanded_size);
    exit(1);
}

static void distribute_shared(const Extraction *x, uint32_t payload, Buffer expanded) {
    size_t position = 0;

    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], payload))
            for (int fork = 0; fork < 2; fork++)
                if (x->records[i].expanded[fork]) {
                    size_t size = x->records[i].expanded[fork];

                    if (position > expanded.n || size > expanded.n - position)
                        die("shared payload layout overflow");

                    write_fork(x, i, fork, expanded.p + position, size);
                    position += size;
                }

    if (position != expanded.n && !catalog_has_vise8_payloads(x->layout))
        die("unassigned bytes in shared payload");
}

static void extract_shared(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];
    size_t expanded_size = shared_expanded_size(x, record->payload);
    size_t packed_size = shared_packed_size(x, record, &expanded_size);

    check_payload(x, record->payload, packed_size);

    Buffer expanded =
        inflate_member(slice(x->data, record->payload, packed_size), x->table, expanded_size);

    distribute_shared(x, record->payload, expanded);
    free(expanded.p);
}

static void extract_vise8_framed(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];
    Buffer expanded = inflate_member(slice(x->data, record->payload, record->packed[0]), x->table,
                                     record->packed[1]);

    if (record->expanded[0] > expanded.n)
        die("VISE 8 framed payload is shorter than its data fork");

    write_fork(x, index, 0, expanded.p, record->expanded[0]);
    free(expanded.p);
}

static void extract_separate_forks(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];
    size_t position = record->payload;

    for (int fork = 0; fork < 2; fork++)
        if (record->packed[fork]) {
            check_payload(x, position, record->packed[fork]);

            Buffer expanded = inflate_member(slice(x->data, position, record->packed[fork]),
                                             x->table, record->expanded[fork]);

            write_fork(x, index, fork, expanded.p, expanded.n);
            free(expanded.p);
            position += record->packed[fork];
        }
}

static void extract_records(const Extraction *x) {
    bool *shared_done = calloc(x->count, sizeof(bool));

    if (!shared_done)
        die("out of memory");

    for (size_t i = 0; i < x->count; i++) {
        const Record *record = &x->records[i];

        if (x->options->list)
            print_record(record, i, x->options->raw_names);
        if (!x->options->out || !record_has_payload(record))
            continue;
        if (record->gap) {
            extract_version_source(x, i);
            continue;
        }

        size_t first = i;
        size_t shared_count = shared_records(x, record->payload, &first);

        if (shared_count == 1 && catalog_has_vise8_payloads(x->layout) && record->packed[0] &&
            record->packed[1] && record->expanded[0] && !record->expanded[1]) {
            extract_vise8_framed(x, i);
        } else if (shared_count > 1) {
            if (i != first || shared_done[first])
                continue;
            shared_done[first] = true;
            extract_shared(x, i);
        } else
            extract_separate_forks(x, i);
    }

    free(shared_done);
}

static Buffer load_data0(Buffer resource, bool *owned, bool *direct, bool *present) {
    Buffer packed = {0}, code = {0};

    *owned = false;
    *direct = false;
    *present = resource_find(resource, "DATA", 0, &packed);

    if (!*present)
        return (Buffer){0};

    if (packed.n < 4 || be32(packed, 0) != UINT32_C(0xa89f000c)) {
        *direct = true;
        return packed;
    }

    if (!resource_find(resource, "CODE", 24, &code) &&
        !resource_find(resource, "CODE", 23, &code) &&
        !resource_find(resource, "CODE", 18, &code))
        die("packed DATA 0 lacks its dictionary resource");

    if (code.n >= 4 && be32(code, 0) == UINT32_C(0xa89f000c)) {
        Buffer dictionary = {0};

        if (!resource_find(resource, "CODE", 25, &dictionary) &&
            !resource_find(resource, "CODE", 1002, &dictionary))
            die("compressed DATA 0 dictionary lacks its own dictionary resource");

        code = dictionary;
    }

    *owned = true;
    return unpack_code(packed, code);
}

static CatalogLayout choose_layout(Buffer data, size_t catalog_offset, Buffer data0,
                                   bool data0_owned, bool direct_table, bool has_data0) {
    bool compressed = be32(data, catalog_offset + 8) != 0;
    uint8_t candidate[256];

    if (!has_data0)
        return CATALOG_VISE8;
    if (direct_table && !compressed)
        return catalog_direct_layout(data, catalog_offset);

    /*
     * Corpus installers from VISE 4.2 and 4.5 use compact records and contain
     * the permutation literally in expanded DATA 0. Later initialization code
     * constructs it instead.
     */
    if (data0_owned && !compressed && find_permutation(data0, candidate) == 1)
        return CATALOG_COMPACT;

    return compressed ? CATALOG_COMPRESSED : CATALOG_NORMAL;
}

static void load_table(Extraction *x, Buffer data0, bool direct_table, bool has_data0) {
    if (!x->options->out)
        return;

    mkdirs(x->options->out);

    if (!has_data0) {
        if (!pef_table(x->data, x->table))
            die("could not find a unique VISE substitution table in the PEF application");
    } else if (direct_table) {
        if (find_permutation(data0, x->table) != 1)
            die("could not identify a unique VISE substitution table");
    } else if (find_permutation(data0, x->table) != 1)
        data0_table(data0, x->table);
}

static void free_records(Record *records, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(records[i].name);
        free(records[i].path);
    }
    free(records);
}

int run_installer(const Options *options, const char *input_path) {
    char *resolved_path = data_fork_path(input_path);
    Buffer input = read_file(resolved_path);
    Buffer resource = {0};
    bool found_resource = false;

    if (input.n < 4 || memcmp(input.p, "SVCT", 4))
        die("not an InstallerVISE data fork; unpack MacBinary, BinHex, or StuffIt first");

#ifdef __APPLE__
    found_resource = read_native_resource_fork(resolved_path, &resource);
#endif
    if (!found_resource)
        found_resource = read_sidecar_resource_fork(resolved_path, &resource);
    if (!found_resource)
        die_missing_resource_fork();

    bool data0_owned, direct_table, has_data0;
    Buffer data0 = load_data0(resource, &data0_owned, &direct_table, &has_data0);

    if (input.n < 0x28)
        die("truncated InstallerVISE SVCT data fork");

    uint32_t version = be32(input, 4);
    uint32_t catalog_offset = be32(input, 0x24);

    if (catalog_offset >= input.n || !span(input, catalog_offset, 4) ||
        memcmp(input.p + catalog_offset, "CVCT", 4))
        die("invalid SVCT catalog offset");

    printf("SVCT version=%" PRIu32 " size=%zu catalog=0x%X\n", version, input.n, catalog_offset);

    size_t catalog_records = be16(input, catalog_offset + 0x10);

    Extraction x = {
        .options = options,
        .data = input,
        .catalog_offset = catalog_offset,
        .layout = choose_layout(input, catalog_offset, data0, data0_owned, direct_table, has_data0),
    };
    Buffer catalog_data = input;
    size_t record_offset = catalog_offset;
    bool catalog_owned = false;

    if (catalog_is_packed(x.layout)) {
        if (!span(input, catalog_offset + 0x14, 4) ||
            memcmp(input.p + catalog_offset + 0x14, "PACK", 4))
            die("packed catalog lacks a PACK header");

        catalog_data = inflate_catalog(input, catalog_offset);
        record_offset = 0;
        catalog_owned = true;

        if (!has_data0)
            x.layout = catalog_pef_layout(catalog_data);
    }

    if (options->list || options->out) {
        x.records = catalog(catalog_data, record_offset, catalog_records, x.layout,
                            options->raw_names, &x.count);
        load_table(&x, data0, direct_table, has_data0);
        extract_records(&x);
        free_records(x.records, x.count);
    }

    if (catalog_owned)
        free(catalog_data.p);
    if (data0_owned)
        free(data0.p);
    free(resource.p);
    free(input.p);
    free(resolved_path);

    return 0;
}
