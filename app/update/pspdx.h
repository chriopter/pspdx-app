#ifndef PSPDX_PSPDX_H
#define PSPDX_PSPDX_H
#include <stddef.h>
#define PSPDX_SCHEMA "https://github.com/chriopter/pspdx/blob/master/schema/v1.pspdx"
#define PSPDX_FILE_MAX 8192
struct pspdx_file {
    char source[256], installdir[42];
    char name[157], author[157], summary[241], license[257], category[12];
};
int pspdx_parse(const char *text, size_t len, struct pspdx_file *out, char *reason,
                size_t reason_size);
int pspdx_install_dir(const char *path);
#endif
