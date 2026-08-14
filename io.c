#include "unvise.h"

#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/xattr.h>
#endif

void die(const char *message) {
    fprintf(stderr, "unvise: %s\n", message);
    exit(1);
}

static void die_errno(const char *path) {
    fprintf(stderr, "unvise: %s: %s\n", path, strerror(errno));
    exit(1);
}

void die_missing_resource_fork(void) {
    die("input is an Installer VISE data fork, but its resource fork is missing; "
        "unpack with 'unar -k hidden' or 'macunpack -f' and keep both forks together");
}

bool span(Buffer b, size_t off, size_t n) {
    return off <= b.n && n <= b.n - off;
}

uint16_t be16(Buffer b, size_t off) {
    if (!span(b, off, 2))
        die("truncated 16-bit field");
    return (uint16_t)((b.p[off] << 8) | b.p[off + 1]);
}

uint32_t be32(Buffer b, size_t off) {
    if (!span(b, off, 4))
        die("truncated 32-bit field");
    return ((uint32_t)b.p[off] << 24) | ((uint32_t)b.p[off + 1] << 16) |
           ((uint32_t)b.p[off + 2] << 8) | b.p[off + 3];
}

Buffer slice(Buffer b, size_t off, size_t n) {
    if (!span(b, off, n))
        die("slice outside input");
    return (Buffer){b.p + off, n};
}

Buffer read_file(const char *path) {
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
    errno = 0;
    size_t read = fread(p, 1, (size_t)length, f);
    if ((size_t)length != read) {
        if (errno)
            die_errno(path);
        fprintf(stderr, "unvise: %s: unexpected end of file\n", path);
        exit(1);
    }
    if (fclose(f))
        die_errno(path);

    return (Buffer){p, (size_t)length};
}

char *data_fork_path(const char *path) {
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
bool read_native_resource_fork(const char *path, Buffer *resource) {
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

static bool read_appledouble(const char *path, Buffer *resource, bool *container) {
    struct stat st;

    *container = false;

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
    *container = true;

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

bool read_sidecar_resource_fork(const char *path, Buffer *resource) {
    /*
     * Look for the portable layouts emitted by the recommended front ends:
     *
     *     Installer + ._Installer       unar -k hidden
     *     Installer.data +
     *         Installer.rsrc           macunpack -f (raw forks)
     *
     * Some unar modes name an AppleDouble container `Installer.rsrc` rather
     * than `._Installer`; its magic distinguishes it from a raw fork.
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

    bool appledouble = false;
    bool found = read_appledouble(hidden, resource, &appledouble);
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

    found = read_appledouble(raw, resource, &appledouble);
    if (!found && appledouble)
        die("AppleDouble sidecar lacks a resource fork");
    if (!found)
        *resource = read_file(raw);
    free(raw);

    return true;
}

static void mkdir_one(const char *path) {
    if (mkdir(path, 0777) && errno != EEXIST)
        die_errno(path);
}

void mkdirs(const char *path) {
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

void make_parent_dir(const char *path) {
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

static size_t add_offset(size_t base, size_t offset, const char *part) {
    if (offset > SIZE_MAX - base) {
        fprintf(stderr, "unvise: resource %s offset overflow\n", part);
        exit(1);
    }
    return base + offset;
}

static size_t indexed_offset(size_t base, size_t index, size_t stride, const char *part) {
    if (index > SIZE_MAX / stride)
        die("resource table index overflow");
    return add_offset(base, index * stride, part);
}

static unsigned resource_count(uint16_t minus_one) {
    return minus_one == UINT16_MAX ? 0 : (unsigned)minus_one + 1;
}

bool resource_find(Buffer r, const char type[4], int wanted, Buffer *result) {
    size_t db = be32(r, 0), mb = be32(r, 4), ml = be32(r, 12);
    size_t map_fields = add_offset(mb, 24, "map header");

    if (!span(r, mb, ml) || !span(r, map_fields, 4))
        die("bad resource map");
    size_t tl = add_offset(mb, be16(r, map_fields), "type list");
    /* The stored count is "types minus one"; 0xffff denotes an empty map. */
    unsigned tc = resource_count(be16(r, tl));
    for (unsigned ti = 0; ti < tc; ti++) {
        size_t te = indexed_offset(add_offset(tl, 2, "type entries"), ti, 8,
                                   "type entry");
        if (!span(r, te, 8))
            die("truncated resource type list");
        if (memcmp(r.p + te, type, 4))
            continue;
        unsigned count = resource_count(be16(r, add_offset(te, 4, "type count")));
        size_t refs = add_offset(tl, be16(r, add_offset(te, 6, "reference list")),
                                 "reference list");
        for (unsigned i = 0; i < count; i++) {
            size_t ref = indexed_offset(refs, i, 12, "reference");
            if (!span(r, ref, 12))
                die("truncated resource reference");
            int id = (int16_t)be16(r, ref);
            if (id != wanted)
                continue;
            size_t rel =
                ((size_t)r.p[ref + 5] << 16) | ((size_t)r.p[ref + 6] << 8) | r.p[ref + 7];
            size_t data = add_offset(db, rel, "data");
            uint32_t n = be32(r, data);
            *result = slice(r, add_offset(data, 4, "data body"), n);
            return true;
        }
    }
    return false;
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

static void write_appledouble(const char *path, const uint8_t *p, size_t n,
                              const uint8_t finder_info[16], bool resource) {
    enum { header_size = 26, entry_size = 12, finder_size = 32 };
    unsigned entries = resource ? 2 : 1;
    size_t finder_offset = header_size + entries * entry_size;
    size_t resource_offset = finder_offset + finder_size;
    size_t total = resource_offset + (resource ? n : 0);

    if (n > UINT32_MAX || total < resource_offset || total > UINT32_MAX)
        die("resource fork is too large for AppleDouble");

    Buffer sidecar = {calloc(total, 1), total};

    if (!sidecar.p)
        die("out of memory");

    /* AppleDouble version 2: ID 9 is Finder info and ID 2 is the resource fork. */
    put_be32(sidecar.p, UINT32_C(0x00051607));
    put_be32(sidecar.p + 4, UINT32_C(0x00020000));
    put_be16(sidecar.p + 24, (uint16_t)entries);
    put_be32(sidecar.p + 26, 9);
    put_be32(sidecar.p + 30, (uint32_t)finder_offset);
    put_be32(sidecar.p + 34, finder_size);
    memcpy(sidecar.p + finder_offset, finder_info, 16);
    if (resource) {
        put_be32(sidecar.p + 38, 2);
        put_be32(sidecar.p + 42, (uint32_t)resource_offset);
        put_be32(sidecar.p + 46, (uint32_t)n);
        memcpy(sidecar.p + resource_offset, p, n);
    }

    char *sidecar_path = appledouble_path(path);

    write_file(sidecar_path, sidecar.p, sidecar.n);
    free(sidecar_path);
    free(sidecar.p);
}

#ifdef __APPLE__
static void write_native_dates(const char *path, uint32_t created, uint32_t modified) {
    enum { mac_to_unix = 2082844800U };
    struct attrlist attributes = {0};
    struct timespec dates[2];
    size_t count = 0;

    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    if (created) {
        attributes.commonattr |= ATTR_CMN_CRTIME;
        dates[count].tv_sec = (time_t)((int64_t)created - mac_to_unix);
        dates[count++].tv_nsec = 0;
    }
    if (modified) {
        attributes.commonattr |= ATTR_CMN_MODTIME;
        dates[count].tv_sec = (time_t)((int64_t)modified - mac_to_unix);
        dates[count++].tv_nsec = 0;
    }
    if (count && setattrlist(path, &attributes, dates, count * sizeof(*dates), 0))
        die_errno(path);
}
#endif

static void write_posix_modified(const char *path, uint32_t modified) {
    enum { mac_to_unix = 2082844800U };
    struct timespec dates[2];

    if (!modified)
        return;
    dates[0].tv_sec = 0;
    dates[0].tv_nsec = UTIME_OMIT;
    dates[1].tv_sec = (time_t)((int64_t)modified - mac_to_unix);
    dates[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, path, dates, 0))
        die_errno(path);
}

static void write_native_finder_info(const char *path, const uint8_t finder_info[16]) {
#ifdef __APPLE__
    uint8_t extended_finder_info[32] = {0};

    memcpy(extended_finder_info, finder_info, 16);
    if (setxattr(path, "com.apple.FinderInfo", extended_finder_info,
                 sizeof(extended_finder_info), 0, 0))
        die_errno(path);
#else
    (void)path;
    (void)finder_info;
#endif
}

void write_output(const Options *o, const char *path, const char *fork, const uint8_t *p, size_t n,
                  const uint8_t finder_info[16], uint32_t created, uint32_t modified) {
    if (o->appledouble) {
        touch_file(path);
        if (!strcmp(fork, "data")) {
            write_file(path, p, n);
            write_appledouble(path, NULL, 0, finder_info, false);
        } else
            write_appledouble(path, p, n, finder_info, true);
        write_posix_modified(path, modified);
        return;
    }

    if (!o->native) {
        write_file(path, p, n);
        write_posix_modified(path, modified);
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

    write_native_finder_info(path, finder_info);
    write_native_dates(path, created, modified);

#else
    (void)path;
    (void)fork;
    (void)p;
    (void)n;
    (void)finder_info;
    (void)created;
    (void)modified;
    die("-n is only supported on macOS");
#endif
}

void write_directory_metadata(const Options *o, const char *path,
                              const uint8_t finder_info[16], uint32_t created,
                              uint32_t modified) {
    if (o->appledouble) {
        write_appledouble(path, NULL, 0, finder_info, false);
        write_posix_modified(path, modified);
        return;
    }
    if (!o->native) {
        write_posix_modified(path, modified);
        return;
    }
#ifdef __APPLE__
    write_native_finder_info(path, finder_info);
    write_native_dates(path, created, modified);
#else
    (void)path;
    (void)finder_info;
    (void)created;
    (void)modified;
    die("-n is only supported on macOS");
#endif
}
