#include "text.h"
/*
 * Sources, with nothing of the GE in it: the rows the view draws and
 * the words its right column says. What a row leads to is
 * session/options.c's.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "session/manage_sources.h"
#include "update/reach.h"

/* A repository is called what GitHub calls it; anything else by its host,
   which is what a person calls a site. */
static void name_of(const char *url, char *out, size_t size) {
    struct source_repo repo;
    if (sources_parse_repo(url, &repo)) {
        snprintf(out, size, "%.24s/%.36s", repo.owner, repo.name);
        return;
    }
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    snprintf(out, size, "%.*s", (int)strcspn(h, "/"), h);
}

static const char *kind_name(enum source_kind k);

void manage_build(struct manage_sources *m, const struct sources *s) {
    m->sources = s;
    m->count = 0;
    struct manage_row *add = &m->row[m->count++];
    memset(add, 0, sizeof(*add));
    add->kind = MANAGE_ADD;
    add->source = -1;
    add->apps = -1;
    snprintf(add->name, sizeof(add->name), "%s", T_SUB_ADD_SOURCE);
    struct manage_row *direct = &m->row[m->count++];
    memset(direct, 0, sizeof(*direct));
    direct->kind = MANAGE_DIRECT;
    direct->source = -1;
    direct->apps = -1;
    snprintf(direct->name, sizeof(direct->name), "%s", T_SET_DIRECT);
    for (int i = 0; i < s->count && m->count < MANAGE_ROWS; i++) {
        const char *url = s->url[i];
        struct manage_row *r = &m->row[m->count++];
        memset(r, 0, sizeof(*r));
        r->kind = MANAGE_SOURCE;
        r->source = i;
        r->skind = sources_kind(url);
        r->apps = reach_apps(url);
        r->loaded_at = reach_loaded_at(url);
        r->state = reach_unreachable(url)    ? MANAGE_UNREACHABLE
                 : reach_offline_copy(url)   ? MANAGE_OFFLINE
                 : r->apps >= 0              ? MANAGE_OK
                                             : MANAGE_NOT_LOADED;
        name_of(url, r->name, sizeof(r->name));
        /* What kind of source it is comes first on the row -- a catalog
           site, a catalog file, a text list, one repository -- since the
           list holds several kinds and a name alone does not say which. */
        const char *s_apps = r->apps == 1 ? "" : "s";
        const char *kind = kind_name(sources_kind(url));
        char rest[48];
        if (r->state == MANAGE_UNREACHABLE)
            snprintf(rest, sizeof(rest), "%s", T_SOURCE_UNREACHABLE);
        else if (r->state == MANAGE_OFFLINE)
            snprintf(rest, sizeof(rest), T_SOURCE_OFFLINE, r->apps, s_apps);
        else if (r->state == MANAGE_OK)
            snprintf(rest, sizeof(rest), T_SOURCE_APPS, r->apps, s_apps);
        else
            snprintf(rest, sizeof(rest), "%s", T_SOURCE_NOT_LOADED);
        snprintf(r->status, sizeof(r->status), "%s, %s", kind, rest);
    }
    if (m->cursor >= m->count) m->cursor = m->count - 1;
    if (m->cursor < 0) m->cursor = 0;
}

void manage_move(struct manage_sources *m, int by) {
    if (m->count > 0) m->cursor = (m->cursor + by + m->count) % m->count;
}

static const struct manage_row *row_at(const struct manage_sources *m, int row) {
    return row >= 0 && row < m->count ? &m->row[row] : NULL;
}

const char *manage_title(const struct manage_sources *m, int row) {
    const struct manage_row *r = row_at(m, row);
    return r ? r->name : "";
}

const char *manage_url(const struct manage_sources *m, int row) {
    const struct manage_row *r = row_at(m, row);
    return r && r->kind == MANAGE_SOURCE ? m->sources->url[r->source] : "";
}

static const char *kind_name(enum source_kind k) {
    return k == SOURCE_REPO ? T_SOURCE_KIND_REPO : k == SOURCE_LIST ? T_SOURCE_KIND_LIST
         : k == SOURCE_CATALOG ? T_SOURCE_KIND_JSON : T_SOURCE_KIND_SITE;
}

static const char *kind_note(enum source_kind k) {
    return k == SOURCE_REPO ? T_SOURCE_NOTE_REPO : k == SOURCE_LIST ? T_SOURCE_NOTE_LIST
         : k == SOURCE_CATALOG ? T_SOURCE_NOTE_JSON : T_SOURCE_NOTE_SITE;
}

void manage_note(const struct manage_sources *m, int row, char *out, size_t size) {
    const struct manage_row *r = row_at(m, row);
    if (!size) return;
    if (!r) { out[0] = '\0'; return; }
    if (r->kind == MANAGE_ADD) { snprintf(out, size, "%s", T_SOURCE_NOTE_ADD); return; }
    if (r->kind == MANAGE_DIRECT) { snprintf(out, size, "%s", T_NOTE_DIRECT); return; }
    /* What went wrong, when something did, says more than what it is: the
       kind is among the facts under it anyway. */
    const char *first = r->state == MANAGE_UNREACHABLE ? T_SOURCE_NOTE_UNREACHABLE
                      : r->state == MANAGE_OFFLINE     ? T_SOURCE_NOTE_OFFLINE
                      : kind_note(sources_kind(manage_url(m, row)));
    snprintf(out, size, "%s %s", first, T_SOURCE_NOTE_KEEP);
}

int manage_facts(const struct manage_sources *m, int row, char facts[][MANAGE_FACT], int max) {
    const struct manage_row *r = row_at(m, row);
    int n = 0;
    if (!r || r->kind != MANAGE_SOURCE) return 0;
    if (n < max)
        snprintf(facts[n++], MANAGE_FACT, T_SOURCE_FACT_KIND, kind_name(sources_kind(manage_url(m, row))));
    if (n < max && r->apps >= 0)
        snprintf(facts[n++], MANAGE_FACT, T_SOURCE_FACT_APPS, r->apps);
    if (n < max && r->state == MANAGE_OFFLINE) {
        snprintf(facts[n++], MANAGE_FACT, "%s", T_SOURCE_FACT_SAVED);
    } else if (n < max && r->loaded_at) {
        time_t at = r->loaded_at;
        struct tm *date = gmtime(&at);
        char when[24] = T_UNKNOWN;
        if (date) strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", date);
        snprintf(facts[n++], MANAGE_FACT, T_SOURCE_FACT_LOADED, when);
    }
    return n;
}
