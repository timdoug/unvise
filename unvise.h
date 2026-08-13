#ifndef UNVISE_H
#define UNVISE_H

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
void write_output(const Options *o, const char *path, const char *fork, const uint8_t *p, size_t n);

Buffer unpack_code(Buffer p, Buffer code);
unsigned find_permutation(Buffer b, uint8_t table[256]);
bool pef_table(Buffer data, uint8_t table[256]);
void data0_table(Buffer d, uint8_t table[256]);
Buffer inflate_member(Buffer packed, const uint8_t table[256], size_t expected);
Buffer inflate_catalog(Buffer data, size_t catalog_offset);

bool catalog_is_packed(CatalogLayout layout);
bool catalog_has_vise8_payloads(CatalogLayout layout);
Record *catalog(Buffer data, size_t offset, CatalogLayout layout, bool raw_names, size_t *count);
void print_quoted(const char *s, bool raw);
char *output_path(const Options *options, Record *records, size_t count, size_t index,
                  const char *fork);
int run_installer(const Options *options, const char *input_path);

#endif
