#include "unvise.h"

typedef struct {
    Buffer fork[2];
    bool have[2], emitted;
} DeferredOutput;

typedef struct {
    const Options *options;
    Buffer data, payload_data;
    uint32_t catalog_offset;
    CatalogLayout layout;
    bool catalog_packed;
    Record *records;
    size_t count;
    uint8_t table[256];
    DeferredOutput *deferred;
} Extraction;

typedef enum {
    PAYLOAD_SEPARATE_FORKS,
    PAYLOAD_SHARED_MEMBER,
    PAYLOAD_OFFSET_MEMBER,
    PAYLOAD_FRAMED_MEMBER
} PayloadLayout;

static bool record_has_payload(const Record *record) {
    return !strcmp(record->tag, "FVCT") && record->file && !record->base_dependent &&
           !record->external &&
           (record->expanded[0] || record->expanded[1]) &&
           (record->packed[0] || record->packed[1]);
}

static bool record_is_empty_file(const Record *record) {
    return !strcmp(record->tag, "FVCT") && record->file && !record->expanded[0] &&
           !record->expanded[1] && !record->packed[0] && !record->packed[1];
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

    if (record->base_dependent)
        fputs(" base-dependent", stdout);
    if (record->external)
        fputs(" external", stdout);
    if (record->segment)
        printf(" segment=%u", record->segment);

    if (record->name) {
        fputs(" name=", stdout);
        print_quoted(stdout, record->name, raw_names);
    }

    putchar('\n');
}

static void check_payload(const Extraction *x, size_t offset, size_t packed_size) {
    if (x->payload_data.p != x->data.p) {
        if (!span(x->payload_data, offset, packed_size))
            die("payload outside companion archive");
        return;
    }
    if (x->catalog_packed && offset >= x->catalog_offset)
        die("installer uses an external web payload archive; the stub alone cannot be extracted");
    if (offset > x->catalog_offset || packed_size > x->catalog_offset - offset)
        die("payload overlaps catalog");
}

static uint32_t fork_crc(uint32_t crc, const uint8_t *data, size_t size) {
    while (size) {
        uInt chunk = size > UINT_MAX ? UINT_MAX : (uInt)size;

        crc = (uint32_t)crc32(crc, data, chunk);
        data += chunk;
        size -= chunk;
    }
    return crc;
}

static void check_fork_crc(const Record *record, const uint8_t *data, const uint8_t *resource) {
    uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);

    if (record->expanded[0])
        crc = fork_crc(crc, data, record->expanded[0]);
    if (record->expanded[1])
        crc = fork_crc(crc, resource, record->expanded[1]);
    if (crc != record->checksum) {
        fputs("unvise: checksum mismatch for ", stderr);
        print_quoted(stderr, record->name ? record->name : "unnamed", false);
        fprintf(stderr, ": expected 0x%08X, got 0x%08X\n", record->checksum, crc);
        exit(1);
    }
}

static void write_fork(const Extraction *x, size_t index, int fork, const uint8_t *data,
                       size_t size) {
    const char *kind = fork ? "rsrc" : "data";
    char *path = output_path(x->options, x->records, index, kind);

    write_output(x->options, path, kind, data, size, x->records[index].finder_info,
                 x->records[index].created, x->records[index].modified);
    free(path);
}

static bool same_deferred_record(const Extraction *x, size_t a, size_t b) {
    for (int fork = 0; fork < 2; fork++) {
        size_t expected_a = x->records[a].expanded[fork];
        size_t expected_b = x->records[b].expanded[fork];

        if (!!expected_a != !!expected_b)
            return false;
        if (expected_a &&
            (x->deferred[a].fork[fork].n != x->deferred[b].fork[fork].n ||
             memcmp(x->deferred[a].fork[fork].p, x->deferred[b].fork[fork].p,
                    x->deferred[a].fork[fork].n)))
            return false;
    }
    return true;
}

static bool deferred_record_complete(const Extraction *x, size_t index) {
    for (int fork = 0; fork < 2; fork++)
        if (x->records[index].expanded[fork] && !x->deferred[index].have[fork])
            return false;
    return true;
}

static void finish_deferred_record(const Extraction *x, size_t index) {
    DeferredOutput *output = &x->deferred[index];
    size_t group = x->records[index].output_group;

    for (size_t i = group; i < index; i++)
        if (x->records[i].output_group == group && x->deferred[i].emitted &&
            same_deferred_record(x, i, index)) {
            for (int fork = 0; fork < 2; fork++) {
                free(output->fork[fork].p);
                output->fork[fork] = (Buffer){0};
            }
            return;
        }

    for (int fork = 0; fork < 2; fork++)
        if (output->have[fork])
            write_fork(x, index, fork, output->fork[fork].p, output->fork[fork].n);
    output->emitted = true;
}

static void emit_fork(const Extraction *x, size_t index, int fork, const uint8_t *data,
                      size_t size) {
    DeferredOutput *output;

    if (x->records[index].output_group == SIZE_MAX) {
        write_fork(x, index, fork, data, size);
        return;
    }

    output = &x->deferred[index];
    output->fork[fork].p = malloc(size ? size : 1);
    if (!output->fork[fork].p)
        die("out of memory");
    memcpy(output->fork[fork].p, data, size);
    output->fork[fork].n = size;
    output->have[fork] = true;

    if (deferred_record_complete(x, index))
        finish_deferred_record(x, index);
}

static void extract_offset_forks(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];
    size_t total = record->packed[1];

    for (int fork = 0; fork < 2; fork++)
        if (record->fork_offset[fork] > total ||
            record->expanded[fork] > total - record->fork_offset[fork])
            die("offset-fork payload layout overflow");

    check_payload(x, record->payload, record->packed[0]);

    Buffer expanded =
        inflate_member(slice(x->payload_data, record->payload, record->packed[0]), x->table,
                       total);
    const uint8_t *data = expanded.p + record->fork_offset[0];
    const uint8_t *resource = expanded.p + record->fork_offset[1];

    check_fork_crc(record, data, resource);

    if (record->expanded[0])
        emit_fork(x, index, 0, data, record->expanded[0]);
    if (record->expanded[1])
        emit_fork(x, index, 1, resource, record->expanded[1]);

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
    bool has_data = false;

    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], record->payload)) {
            if (x->records[i].expanded[0])
                has_data = true;
        }

    /*
     * A shared group containing data forks uses field 0 as its packed size;
     * field 1 is the complete expanded member size. A resource-only group
     * instead uses field 1 as its packed size and the declared fork sizes give
     * the expanded size. Late data-bearing layouts use the first form.
     */
    if (catalog_has_late_payloads(x->layout) && has_data) {
        *expanded_size = record->packed[1];
        if (record->packed[0] && *expanded_size)
            return record->packed[0];
    } else if (has_data && record->packed[0] && record->packed[1]) {
        *expanded_size = record->packed[1];
        return record->packed[0];
    } else if (!has_data && record->packed[0] && record->packed[1]) {
        *expanded_size = record->packed[1];
        return record->packed[0];
    } else if (!has_data && record->packed[1]) {
        return record->packed[1];
    }

    fprintf(stderr,
            "unvise: unknown shared payload layout at 0x%X "
            "(0x%X, 0x%X, expanded 0x%zx)\n",
            record->payload, record->packed[0], record->packed[1], *expanded_size);
    exit(1);
}

static void extract_shared_resource_endpoint(const Extraction *x, uint32_t payload) {
    size_t packed_size = 0;
    size_t expanded_size = 0;
    size_t endpoint = SIZE_MAX;

    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], payload)) {
            const Record *record = &x->records[i];

            if (record->expanded[0] || record->packed[0] || !record->expanded[1] ||
                !record->packed[1])
                die("invalid resource-only shared payload");
            if (record->packed[1] > packed_size)
                packed_size = record->packed[1];
            if (record->expanded[1] > expanded_size)
                expanded_size = record->expanded[1];
            if (record->packed[1] == packed_size)
                endpoint = i;
        }

    if (endpoint == SIZE_MAX || x->records[endpoint].subtype == 6)
        die("invalid resource-only shared payload endpoint");
    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], payload) && i != endpoint &&
            x->records[i].subtype != 6)
            die("unknown resource-only shared payload layout");

    check_payload(x, payload, packed_size);
    Buffer expanded =
        inflate_member(slice(x->payload_data, payload, packed_size), x->table, expanded_size);

    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], payload)) {
            const Record *record = &x->records[i];

            /*
             * Shorter endpoints are installer-private update references into
             * the following complete resource member. They do not terminate
             * a DEFLATE stream and their +0x54 field is not a fork CRC.
             */
            if (i == endpoint) {
                check_fork_crc(record, NULL, expanded.p);
                emit_fork(x, i, 1, expanded.p, record->expanded[1]);
            }
        }

    free(expanded.p);
}

static PayloadLayout payload_layout(const Extraction *x, const Record *record,
                                    size_t shared_count) {
    size_t member_end = 0;

    if (record->has_fork_offsets)
        for (int fork = 0; fork < 2; fork++) {
            size_t offset = record->fork_offset[fork];

            if (record->expanded[fork] > SIZE_MAX - offset)
                die("fork offset overflow");
            if (offset + record->expanded[fork] > member_end)
                member_end = offset + record->expanded[fork];
        }

    if (shared_count > 1)
        return PAYLOAD_SHARED_MEMBER;

    if (catalog_has_late_payloads(x->layout) && record->packed[0] &&
        record->packed[1] && record->expanded[0] && !record->expanded[1])
        return PAYLOAD_FRAMED_MEMBER;

    if (record->has_fork_offsets && record->packed[0] && member_end &&
        record->packed[1] >= member_end)
        return PAYLOAD_OFFSET_MEMBER;

    return PAYLOAD_SEPARATE_FORKS;
}

static void distribute_shared(const Extraction *x, uint32_t payload, Buffer expanded) {
    size_t position = 0;
    size_t covered_end = 0;
    bool uses_offsets = false;

    for (size_t i = 0; i < x->count; i++)
        if (same_payload(&x->records[i], payload)) {
            const Record *record = &x->records[i];
            const uint8_t *forks[2] = {NULL, NULL};

            for (int fork = 0; fork < 2; fork++)
                if (record->expanded[fork]) {
                    size_t size = record->expanded[fork];

                    if (record->has_fork_offsets) {
                        uses_offsets = true;
                        position = record->fork_offset[fork];
                    }

                    if (position > expanded.n || size > expanded.n - position)
                        die("shared payload layout overflow");

                    forks[fork] = expanded.p + position;
                    position += size;
                    if (position > covered_end)
                        covered_end = position;
                }

            check_fork_crc(record, forks[0], forks[1]);
            for (int fork = 0; fork < 2; fork++)
                if (forks[fork])
                    emit_fork(x, i, fork, forks[fork], record->expanded[fork]);
        }

    if (covered_end != expanded.n && !uses_offsets && !catalog_has_late_payloads(x->layout))
        die("unassigned bytes in shared payload");
}

static void extract_shared(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];

    if (catalog_has_late_payloads(x->layout) && !record->expanded[0] && !record->packed[0]) {
        extract_shared_resource_endpoint(x, record->payload);
        return;
    }

    size_t expanded_size = shared_expanded_size(x, record->payload);
    size_t packed_size = shared_packed_size(x, record, &expanded_size);

    check_payload(x, record->payload, packed_size);

    Buffer expanded =
        inflate_member(slice(x->payload_data, record->payload, packed_size), x->table,
                       expanded_size);

    distribute_shared(x, record->payload, expanded);
    free(expanded.p);
}

static void extract_framed(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];

    check_payload(x, record->payload, record->packed[0]);

    Buffer expanded =
        inflate_member(slice(x->payload_data, record->payload, record->packed[0]), x->table,
                       record->packed[1]);

    if (record->fork_offset[0] > expanded.n ||
        record->expanded[0] > expanded.n - record->fork_offset[0])
        die("framed payload is shorter than its data fork");

    check_fork_crc(record, expanded.p + record->fork_offset[0], NULL);
    emit_fork(x, index, 0, expanded.p + record->fork_offset[0], record->expanded[0]);
    free(expanded.p);
}

static void extract_separate_forks(const Extraction *x, size_t index) {
    const Record *record = &x->records[index];
    size_t position = record->payload;
    Buffer expanded[2] = {{0}, {0}};

    for (int fork = 0; fork < 2; fork++)
        if (record->packed[fork]) {
            check_payload(x, position, record->packed[fork]);

            expanded[fork] = inflate_member(slice(x->payload_data, position, record->packed[fork]),
                                            x->table, record->expanded[fork]);
            position += record->packed[fork];
        }

    check_fork_crc(record, expanded[0].p, expanded[1].p);
    for (int fork = 0; fork < 2; fork++)
        if (expanded[fork].p) {
            emit_fork(x, index, fork, expanded[fork].p, expanded[fork].n);
            free(expanded[fork].p);
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
        if (!x->options->out)
            continue;
        if (record_is_empty_file(record)) {
            check_fork_crc(record, NULL, NULL);
            emit_fork(x, i, 0, (const uint8_t *)"", 0);
            continue;
        }
        if (!record_has_payload(record))
            continue;
        size_t first = i;
        size_t shared_count = shared_records(x, record->payload, &first);
        PayloadLayout layout = payload_layout(x, record, shared_count);

        if (layout == PAYLOAD_FRAMED_MEMBER) {
            extract_framed(x, i);
        } else if (layout == PAYLOAD_OFFSET_MEMBER) {
            extract_offset_forks(x, i);
        } else if (layout == PAYLOAD_SHARED_MEMBER) {
            if (i != first || shared_done[first])
                continue;
            shared_done[first] = true;
            extract_shared(x, i);
        } else {
            extract_separate_forks(x, i);
        }
    }

    free(shared_done);

    for (size_t i = 0; i < x->count; i++)
        for (int fork = 0; fork < 2; fork++)
            free(x->deferred[i].fork[fork].p);
}

static void write_directories(const Extraction *x) {
    size_t output_length = strlen(x->options->out);

    /*
     * Records are preorder, so reverse order applies child metadata before
     * parent metadata. Creating a child or its AppleDouble sidecar therefore
     * cannot subsequently disturb the parent's restored modification time.
     */
    for (size_t i = x->count; i; i--) {
        const Record *record = &x->records[i - 1];

        if (strcmp(record->tag, "DVCT"))
            continue;
        size_t path_length = output_length + 1 + strlen(record->path) + 1;
        char *path = malloc(path_length);

        if (!path)
            die("out of memory");
        snprintf(path, path_length, "%s/%s", x->options->out, record->path);
        mkdirs(path);
        write_directory_metadata(x->options, path, record->finder_info,
                                 record->created, record->modified);
        free(path);
    }
}

static Buffer load_data0(Buffer resource, bool *owned, bool *present) {
    Buffer packed = {0}, code = {0};

    *owned = false;
    *present = resource_find(resource, "DATA", 0, &packed);

    if (!*present)
        return (Buffer){0};

    if (packed.n < 4 || be32(packed, 0) != UINT32_C(0xa89f000c))
        return packed;

    bool found_code = resource_find(resource, "CODE", 24, &code);

    if (!found_code) {
        Buffer relocated = {0};

        /* VISE 5.5.1 can relocate the identical unpacker to CODE 5002. */
        if (resource_find(resource, "CODE", 5002, &relocated) &&
            span(relocated, 0x10, 8) && !memcmp(relocated.p + 0x10, "\xa8\x9fVISE", 6)) {
            code = relocated;
            found_code = true;
        }
    }
    if (!found_code && !resource_find(resource, "CODE", 23, &code) &&
        !resource_find(resource, "CODE", 18, &code) &&
        !resource_find(resource, "CODE", 20, &code))
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

static CatalogLayout choose_layout(Buffer data, size_t catalog_offset, uint8_t revision) {
    bool compressed = be32(data, catalog_offset + 8) != 0;

    return compressed ? catalog_packed_layout(revision) :
                        catalog_uncompressed_layout(data.p[0x12], revision);
}

static void load_table(Extraction *x, Buffer data0, bool has_data0) {
    if (!x->options->out)
        return;

    mkdirs(x->options->out);

    if (!has_data0) {
        if (!pef_table(x->data, x->table))
            die("could not find a unique VISE substitution table in the PEF application");
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

static void skip_short_payload_headers(Extraction *x) {
    bool decided = false, embedded = false;

    if (x->layout != CATALOG_SHORT)
        return;

    for (size_t i = 0; i < x->count; i++) {
        Record *record = &x->records[i];

        if (!record->file || (!record->packed[0] && !record->packed[1]))
            continue;

        bool has_header = span(x->payload_data, record->payload, 4) &&
                          !memcmp(x->payload_data.p + record->payload, "FVCT", 4);
        if (!decided) {
            embedded = has_header;
            decided = true;
        } else if (has_header != embedded)
            die("inconsistent short catalog payload headers");
        if (!embedded)
            continue;

        size_t header_size = record->end - record->off;
        if (!span(x->payload_data, record->payload, header_size))
            die("truncated embedded FVCT payload header");
        if (record->payload > UINT32_MAX - header_size)
            die("short catalog payload offset overflow");
        record->payload += (uint32_t)header_size;
    }
}

static Buffer companion_archive(const char *input_path, const char *resolved_path,
                                Buffer input, bool *owned) {
    size_t n = strlen(input_path);
    struct stat st;

    *owned = false;
    if (strcmp(input_path, resolved_path))
        return input;

    char *path = malloc(n + sizeof(".data"));
    if (!path)
        die("out of memory");
    strcpy(path, input_path);
    strcat(path, ".data");

    if (stat(path, &st)) {
        if (errno != ENOENT) {
            fprintf(stderr, "unvise: %s: %s\n", path, strerror(errno));
            exit(1);
        }
        free(path);
        return input;
    }

    Buffer companion = read_file(path);
    free(path);
    Buffer companion_resource = {0};
    bool have_companion_resource = false;

    unwrap_transport(&companion, &companion_resource, &have_companion_resource);
    if (have_companion_resource)
        free(companion_resource.p);
    if (companion.n >= 4 && !memcmp(companion.p, "SVCT", 4)) {
        free(companion.p);
        return input;
    }

    *owned = true;
    return companion;
}

int run_installer(const Options *options, const char *input_path) {
    char *resolved_path = data_fork_path(input_path);
    Buffer input = read_file(resolved_path);
    Buffer resource = {0};
    bool found_resource = false;

#ifdef __APPLE__
    found_resource = read_native_resource_fork(resolved_path, &resource);
#endif
    if (!found_resource)
        found_resource = read_sidecar_resource_fork(resolved_path, &resource);
    unwrap_transport(&input, &resource, &found_resource);

    if (input.n < 4 || memcmp(input.p, "SVCT", 4))
        die("not an Installer VISE application; unpack any StuffIt layer first");
    if (!found_resource)
        die_missing_resource_fork();

    bool data0_owned, has_data0, payload_owned;
    Buffer data0 = load_data0(resource, &data0_owned, &has_data0);
    Buffer payload_data = input;

    payload_owned = false;
    if (options->out)
        payload_data = companion_archive(input_path, resolved_path, input, &payload_owned);

    if (input.n < 0x28)
        die("truncated Installer VISE SVCT data fork");

    uint32_t version = be32(input, 4);
    uint8_t revision = input.p[0x13];
    uint32_t segment_count = be32(input, 0x14);
    uint32_t catalog_offset = be32(input, 0x24);

    if (catalog_offset >= input.n || !span(input, catalog_offset, 4) ||
        memcmp(input.p + catalog_offset, "CVCT", 4))
        die("invalid SVCT catalog offset");

    printf("SVCT version=%" PRIu32 " size=%zu catalog=0x%X\n", version, input.n, catalog_offset);

    size_t catalog_records = be16(input, catalog_offset + 0x10);

    Extraction x = {
        .options = options,
        .data = input,
        .payload_data = payload_data,
        .catalog_offset = catalog_offset,
        .layout = choose_layout(input, catalog_offset, revision),
        .catalog_packed = be32(input, catalog_offset + 8) != 0,
    };
    Buffer catalog_data = input;
    size_t record_offset = catalog_offset;
    bool catalog_owned = false;

    if (x.catalog_packed) {
        if (!span(input, catalog_offset + 0x14, 4) ||
            memcmp(input.p + catalog_offset + 0x14, "PACK", 4))
            die("packed catalog lacks a PACK header");

        catalog_data = inflate_catalog(input, catalog_offset);
        record_offset = 0;
        catalog_owned = true;

    }

    if (options->list || options->out) {
        x.records = catalog(catalog_data, record_offset, catalog_records, x.layout,
                            options->raw_names, revision, &x.count);
        skip_short_payload_headers(&x);
        for (size_t i = 0; i < x.count; i++) {
            Record *record = &x.records[i];
            size_t packed_size = (size_t)record->packed[0] + record->packed[1];
            bool outside_main_segment = record->payload > catalog_offset ||
                                        packed_size > catalog_offset - record->payload;

            if (segment_count > 1 && record->file &&
                (record->segment != 1 || outside_main_segment) &&
                (record->expanded[0] || record->expanded[1]) &&
                (record->packed[0] || record->packed[1]))
                record->external = true;
        }
        x.deferred = calloc(x.count, sizeof(*x.deferred));
        if (!x.deferred)
            die("out of memory");
        load_table(&x, data0, has_data0);
        extract_records(&x);
        if (options->out)
            write_directories(&x);
        free(x.deferred);
        free_records(x.records, x.count);
    }

    if (catalog_owned)
        free(catalog_data.p);
    if (data0_owned)
        free(data0.p);
    if (payload_owned)
        free(payload_data.p);
    free(resource.p);
    free(input.p);
    free(resolved_path);

    return 0;
}
