#ifndef UNVISE_H
#define UNVISE_H

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
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

typedef struct {
    uint8_t *p;
    size_t n;
} Buffer;

typedef struct {
    size_t off, end;
    size_t output_group;
    char tag[5];
    char *name, *path;
    bool file, has_fork_offsets;
    uint32_t parent, dir_id, payload, checksum, created, modified;
    uint32_t record_flags;
    uint16_t depth, fixed_size;
    uint32_t packed[2], expanded[2];
    uint32_t fork_offset[2];
    uint8_t finder_info[16];
} Record;

typedef struct {
    bool list, native, appledouble, raw_names;
    const char *out;
} Options;

typedef enum {
    /* These are record layouts, not InstallerVISE version numbers. */
    CATALOG_LITE,
    CATALOG_COMPACT,
    CATALOG_NORMAL,
    CATALOG_COMPRESSED,
    CATALOG_VISE8,
} CatalogLayout;

void die(const char *message);
void die_missing_resource_fork(void);
bool span(Buffer b, size_t off, size_t n);
uint16_t be16(Buffer b, size_t off);
uint32_t be32(Buffer b, size_t off);
Buffer slice(Buffer b, size_t off, size_t n);
Buffer read_file(const char *path);
char *data_fork_path(const char *path);
#ifdef __APPLE__
bool read_native_resource_fork(const char *path, Buffer *resource);
#endif
bool read_sidecar_resource_fork(const char *path, Buffer *resource);
void mkdirs(const char *path);
void make_parent_dir(const char *path);
bool resource_find(Buffer r, const char type[4], int wanted, Buffer *result);
void write_output(const Options *o, const char *path, const char *fork, const uint8_t *p, size_t n,
                  const uint8_t finder_info[16], uint32_t created, uint32_t modified);
void write_directory_metadata(const Options *o, const char *path,
                              const uint8_t finder_info[16], uint32_t created,
                              uint32_t modified);

Buffer unpack_code(Buffer p, Buffer code);
unsigned find_permutation(Buffer b, uint8_t table[256]);
bool pef_table(Buffer data, uint8_t table[256]);
void data0_table(Buffer d, uint8_t table[256]);
Buffer inflate_member(Buffer packed, const uint8_t table[256], size_t expected);
Buffer inflate_catalog(Buffer data, size_t catalog_offset);

bool catalog_is_packed(CatalogLayout layout);
bool catalog_has_vise8_payloads(CatalogLayout layout);
CatalogLayout catalog_compressed_layout(uint8_t revision);
CatalogLayout catalog_uncompressed_layout(uint8_t revision);
Record *catalog(Buffer data, size_t offset, size_t expected, CatalogLayout layout, bool raw_names,
                uint8_t revision, size_t *count);
void print_quoted(FILE *f, const char *s, bool raw);
char *output_path(const Options *options, Record *records, size_t index, const char *fork);
int run_installer(const Options *options, const char *input_path);

#endif
