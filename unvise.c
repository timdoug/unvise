#include "unvise.h"

/*
 * unvise decodes the InstallerVISE layer of a classic Macintosh installer.
 * MacBinary, BinHex, and StuffIt are transport/archive formats outside its
 * scope; remove them first with unar or macutils while preserving both forks.
 *
 * The implementation is tested against 36 freeware, shareware, demo, and
 * InstallerVISE self-installer archives made with Lite 3.6 and versions 4.2
 * through 8.5. It remains reverse-engineered: password-protected
 * members, inferred catalog boundaries, and Active Install external payloads
 * are documented limitations.
 */

static void usage(FILE *f) {
    fprintf(f, "usage: unvise [-l] [-x DIR] [-a | -n] [-r] INSTALLER\n"
               "  -l        list files without extracting\n"
               "  -x DIR    extract files into DIR\n"
               "  -a        write ._name AppleDouble sidecars\n"
               "  -n        use native macOS resource forks\n"
               "  -r        preserve MacRoman filename bytes\n"
               "  -h        show this help\n");
}
static int parse_options(int argc, char **argv, Options *options, const char **input_path) {
    *options = (Options){0};
    *input_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l"))
            options->list = true;
        else if (!strcmp(argv[i], "-x") && i + 1 < argc)
            options->out = argv[++i];
        else if (!strcmp(argv[i], "-n"))
            options->native = true;
        else if (!strcmp(argv[i], "-a"))
            options->appledouble = true;
        else if (!strcmp(argv[i], "-r"))
            options->raw_names = true;
        else if (!strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] == '-' || *input_path) {
            usage(stderr);
            return 2;
        } else
            *input_path = argv[i];
    }

    if (!*input_path) {
        usage(stderr);
        return 2;
    }

    return -1;
}

static void validate_options(const Options *options) {
    if (options->native && options->appledouble)
        die("-n and -a are mutually exclusive");

#ifndef __APPLE__
    if (options->native)
        die("-n is only supported on macOS");
#else
    if (options->raw_names && options->out)
        die("-r extraction is not supported by modern macOS filesystems");
#endif
}

int main(int argc, char **argv) {
    Options options;
    const char *input_path;
    int status = parse_options(argc, argv, &options, &input_path);

    if (status >= 0)
        return status;

    validate_options(&options);
    return run_installer(&options, input_path);
}
