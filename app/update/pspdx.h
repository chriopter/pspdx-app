#ifndef PSPDX_PSPDX_H
#define PSPDX_PSPDX_H
#include <stddef.h>
#define PSPDX_SCHEMA "https://chriopter.github.io/pspdx/schema/pspdx-v1.json"
/* A file of every field at its longest, each character four bytes, is about
   13 KB: a 2500-character description alone can be 10 KB. Anything past this
   is not a .pspdx. */
#define PSPDX_FILE_MAX 16384
/* An address the format allows, 255 characters of up to four bytes each. */
#define PSPDX_URL_SIZE (255 * 4 + 1)
/* Eight tags of at most 24 characters, each character at most four bytes,
   kept one after the other with a newline between them: a tag holds no
   control character, so a newline can only ever be the separator. */
#define PSPDX_TAGS 8
#define PSPDX_TAGS_TEXT (PSPDX_TAGS * (24 * 4 + 1))
/* The category is one such word. */
#define PSPDX_CATEGORY_SIZE (24 * 4 + 1)
/* The byte sizes fit the character limits of schema/pspdx-v1.json at four
   bytes a character. The description is checked and not kept here, since
   it is the one field of kilobytes and this lives on stacks; whoever shows
   it asks pspdx_description for it. */
struct pspdx_file {
    char source[PSPDX_URL_SIZE], installdir[42], listed_by[PSPDX_URL_SIZE];
    char name[161], author[241], summary[241], license[241];
    char type[12];              /* homebrew, plugin or iso; homebrew when the file says nothing */
    char tags[PSPDX_TAGS_TEXT]; /* newline between them */
    char category[PSPDX_CATEGORY_SIZE]; /* the one group it names, empty when none */
    char id[96];                /* derived, never written in the file */
};
/* Holds a .pspdx to version 1. What the file leaves to a rule is filled in
   the way the catalog builder fills it: the type, the install directory of a
   GitHub repository, and the id. */
int pspdx_parse(const char *text, size_t len, struct pspdx_file *out, char *reason,
                size_t reason_size);
int pspdx_install_dir(const char *path);
/* The folder a homebrew goes to when its file names none: the repository's
   name from GitHub (repo), or else the app's name with every character a
   folder cannot hold left out; cut at 32 either way. Not checked here. */
void pspdx_default_dir(const char *repo, const char *name, char *out, size_t size);
/* Whether this client can install what a file of this type describes. */
int pspdx_type_installable(const char *type);
/* Whether one of the newline-separated tags is exactly word. */
int pspdx_has_tag(const char *tags, const char *word);
/* The description of a file pspdx_parse took, on the heap, or NULL when it
   has none or is not one. The caller frees it. */
char *pspdx_description(const char *text, size_t len);
/* How long a string is in the characters the format counts: code points of
   well-formed UTF-8, or -1, and -1 too for a control character, but for a
   newline where newline says it may stand. */
int pspdx_characters(const char *s, int newline);
/* A string snprintf may have cut: a character left without its last bytes
   at the end is taken off, so what is drawn is never half a letter. */
void pspdx_utf8_mend(char *s);
#endif
