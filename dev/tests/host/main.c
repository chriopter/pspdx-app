#include "install/pluginlist.h"
#include "install/state.h"
#include "pspiofilemgr.h"
#include "session/cable.h"
#include "session/manage_sources.h"
#include "session/view.h"
#include "update/assets.h"
#include "update/catalog.h"
#include "update/inbox.h"
#include "update/presets.h"
#include "update/reach.h"
#include "update/sources.h"
#include "gui/wrap.h"
#include "gui/cloud.h"
#include "util/storage.h"
#include "util/pbp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void host_fault(long);
unsigned host_operations(void);
static struct catalog catalog;
/* Exercise installer cancellation at specific download/unpack boundaries. */
static const char *install_phase;
static unsigned progress_calls;
static void test_phase(void *ctx, const char *phase) {
    (void)ctx;
    install_phase = phase;
    progress_calls = 0;
}
static void test_progress(void *ctx, size_t done, size_t total) {
    (void)ctx; (void)done; (void)total;
    const char *phase = getenv("CANCEL_PHASE");
    const char *after = getenv("CANCEL_AFTER");
    if (phase && install_phase && !strcmp(phase, install_phase) &&
        ++progress_calls >= (unsigned)(after ? atoi(after) : 1))
        install_abort();
}
/* The host's font: every character one unit wide, so a width is a count of
   characters and a test can say which line a word lands on. */
static float characters(void *ctx, const char *text, size_t len) {
    (void)ctx;
    float n = 0;
    for (size_t i = 0; i < len; i++)
        n += ((unsigned char)text[i] & 0xC0) != 0x80;
    return n;
}
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    storage_init(getenv("DEVICE") ? getenv("DEVICE") : "ms0:/PSP/GAME/PSPDX/EBOOT.PBP");
    state_load();
    /* A power cut after that many mutating calls, for any command; recover,
       install and remove also take it as their argument. */
    host_fault(getenv("FAULT") ? atol(getenv("FAULT")) : 0);
    if (!strcmp(argv[1], "storage")) {
        printf("%s internal=%d card=%d\n", storage_device(),
               storage_device_available("ef0:"), storage_device_available("ms0:"));
        return 0;
    }
    if (!strcmp(argv[1], "installed-path")) {
        char path[160];
        if (argc < 3 || pbp_installed_path(argv[2], path, sizeof(path)) < 0) return 1;
        puts(path);
        return 0;
    }
    if (!strcmp(argv[1], "parse")) {
        char *raw = NULL;
        struct pspdx_file f;
        char why[100];
        int n = storage_read(argv[2], &raw, PSPDX_FILE_MAX);
        int rc = n < 0 ? -1 : pspdx_parse(raw, n, &f, why, sizeof(why));
        free(raw);
        if (rc < 0) {
            fprintf(stderr, "%s\n", n < 0 ? "unreadable" : why);
            return 1;
        }
        /* What the file leaves to a rule, as the parser filled it in. */
        for (char *p = f.tags; *p; p++)
            if (*p == '\n') *p = ',';
        printf("%s|%s|%s|%s\n", f.installdir, f.type, f.id, f.tags);
        return 0;
    }
    if (!strcmp(argv[1], "wrap")) {
        /* wrap <width> <max bytes> <max lines> <file>: the lines, one JSON
           string each, as the details band would draw them. */
        char *text = NULL;
        int n = storage_read(argv[5], &text, 65535);
        if (n < 0)
            return 2;
        static struct wrap_line lines[4096];
        int most = atoi(argv[4]) < 4096 ? atoi(argv[4]) : 4096;
        int count = wrap_text(text, (float)atof(argv[2]), (size_t)atol(argv[3]), characters, NULL,
                              lines, most);
        for (int i = 0; i < count; i++) {
            putchar('"');
            for (int k = 0; k < lines[i].len; k++) {
                char c = text[lines[i].start + k];
                if (c == '"' || c == '\\')
                    putchar('\\');
                putchar(c);
            }
            puts("\"");
        }
        free(text);
        return 0;
    }
    if (!strcmp(argv[1], "pluginlist")) {
        /* pluginlist <state|add|on|off|blank> <file> <path>: the one write
           the unit works out for a PLUGINS.TXT on the host, put into the
           file where it says and nowhere else; then what the unit
           answered, and what the list says of the plugin at path. */
        char *text = calloc(1, 1 << 20);
        FILE *f = fopen(argv[3], "rb");
        size_t len = f ? fread(text, 1, 1 << 20, f) : 0;
        if (f) fclose(f);
        struct pluginlist_write w = {0};
        int rc = !strcmp(argv[2], "add")     ? pluginlist_add(text, len, argv[4], &w)
                 : !strcmp(argv[2], "on")    ? pluginlist_switch(text, len, argv[4], 1, &w)
                 : !strcmp(argv[2], "off")   ? pluginlist_switch(text, len, argv[4], 0, &w)
                 : !strcmp(argv[2], "blank") ? pluginlist_blank(text, len, argv[4], &w) : -9;
        if (rc == (strcmp(argv[2], "add") ? 1 : 0)) {
            f = fopen(argv[3], len ? "r+b" : "wb");
            fseek(f, (long)w.at, SEEK_SET);
            fwrite(w.bytes, 1, w.n, f);
            fclose(f);
            memcpy(text + w.at, w.bytes, w.n);
            if (w.at + w.n > len) len = w.at + w.n;
        }
        printf("%d %d\n", rc, pluginlist_state(text, len, argv[4]));
        free(text);
        return 0;
    }
    if (!strcmp(argv[1], "cable")) {
        /* cable <choice 0-3> [use|look]: whether the start would ask how to
           connect, with that answer remembered; with "use", the cable chosen:
           what came of it, and the line it said; with "look", the gateway
           looked for: what was found, and what is asked then. */
        enum cable choice = (enum cable)atoi(argv[2]);
        if (argc > 3 && !strcmp(argv[3], "look")) {
            cable_start(choice);
            enum cable_look look = cable_look();
            printf("%d %d %d\n", cable_ready(), look, cable_after_look(choice, look));
            return 0;
        }
        if (argc > 3) {
            char said[96];
            int rc = cable_use(said, sizeof(said));
            printf("%d %d %s\n", rc, cable_ready(), said);
            return 0;
        }
        int asks = cable_asks(choice);
        if (!asks) cable_start(choice);
        printf("%d %d %d\n", asks, cable_installed(), cable_ready());
        return 0;
    }
    if (!strcmp(argv[1], "plugin")) {
        /* plugin <id> [on|off]: whether an installed plugin is turned on,
           after turning it on or off where the test says so. */
        int rc = argc > 3 ? plugin_switch(argv[2], !strcmp(argv[3], "on")) : plugin_enabled(argv[2]);
        printf("%d\n", rc);
        if (rc < 0 && argc > 3 && plugin_refused()[0])
            fprintf(stderr, "why: %s\n", plugin_refused());
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "recover")) {
        if (argc > 2)
            host_fault(atol(argv[2]));
        install_recover();
        return 0;
    }
    if (!strcmp(argv[1], "fetch")) {
        catalog_offline(getenv("OFFLINE") != NULL);
        if (getenv("FORCE"))
            catalog_force_sources();
        int rc = catalog_fetch(&catalog);
        catalog_check_updates(&catalog);
        for (int i = 0; i < catalog.count; i++)
            printf("%s %s %d %d %d\n", catalog.apps[i].id, txt(catalog.apps[i].release.version),
                   catalog.apps[i].fresh, catalog.apps[i].state, catalog.apps[i].unsupported);
        /* What the status line says once the fetch is through. */
        if (catalog.collision[0])
            fprintf(stderr, "status: %s\n", catalog.collision);
        if (rc < 0)
            fprintf(stderr, "status: %s\n", catalog_too_large() ? "too large" : "unreachable");
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "rows")) {
        /* rows: every row after a fetch and a check, id|name|folder|state. */
        catalog_offline(getenv("OFFLINE") != NULL);
        if (getenv("FORCE"))
            catalog_force_sources();
        int rc = catalog_fetch(&catalog);
        catalog_check_updates(&catalog);
        for (int i = 0; i < catalog.count; i++)
            printf("%s|%s|%s|%d|%d\n", catalog.apps[i].id, catalog.apps[i].name,
                   catalog.apps[i].release.dir, catalog.apps[i].state,
                   catalog_new_build(&catalog.apps[i]));
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "media")) {
        /* media <id>: the four addresses the entry's pictures and sounds
           resolved to, one a line, empty where there is none. */
        catalog_fetch(&catalog);
        for (int i = 0; i < catalog.count; i++)
            if (!strcmp(catalog.apps[i].id, argv[2])) {
                printf("%s\n%s\n%s\n%s\n", txt(catalog.apps[i].icon), txt(catalog.apps[i].screenshot),
                       txt(catalog.apps[i].video), txt(catalog.apps[i].sound));
                return 0;
            }
        return 2;
    }
    if (!strcmp(argv[1], "release")) {
        /* release <id>: the version, the zip and whether the file pinned it. */
        catalog_fetch(&catalog);
        for (int i = 0; i < catalog.count; i++)
            if (!strcmp(catalog.apps[i].id, argv[2])) {
                printf("%s %s %d\n", txt(catalog.apps[i].release.version), txt(catalog.apps[i].release.url),
                       catalog.apps[i].release.pinned);
                return 0;
            }
        return 2;
    }
    if (!strcmp(argv[1], "sha")) {
        /* sha <id>: the hash the entry holds the download to, "-" for none,
           and the zip it is the hash of. */
        catalog_fetch(&catalog);
        for (int i = 0; i < catalog.count; i++)
            if (!strcmp(catalog.apps[i].id, argv[2])) {
                const struct entry_release *m = &catalog.apps[i].release;
                int hashed = 0;
                for (int k = 0; k < 32; k++) hashed |= m->sha256[k];
                if (hashed)
                    for (int k = 0; k < 32; k++) printf("%02x", m->sha256[k]);
                else
                    printf("-");
                printf(" %s\n", txt(m->url));
                return 0;
            }
        return 2;
    }
    if (!strcmp(argv[1], "drop")) {
        /* drop <url>: the gear's delete of a source. */
        int rc = sources_remove(argv[2]);
        printf("%d\n", rc);
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "ids")) {
        /* ids <url> [<source> <name>]: the id a repository makes, and
           the one a source's host and a name make, each "-" where there is
           none. */
        struct source_repo repo;
        char id[PSPDX_ID_SIZE], url[SOURCE_URL];
        int github = sources_parse_repo(argv[2], &repo);
        if (github)
            sources_repo_url(&repo, url, sizeof(url));
        printf("%s %s\n", github && sources_repo_id(&repo, id, sizeof(id)) == 0 ? id : "-",
               github ? url : "-");
        if (argc > 4)
            printf("%s\n", sources_host_id(argv[3], argv[4], id, sizeof(id)) == 0 ? id : "-");
        return 0;
    }
    if (!strcmp(argv[1], "latest")) {
        /* latest <id>: what the record last heard, or -1 when it is refused. */
        struct manifest m;
        if (state_latest(argv[2], &m) < 0) {
            puts("-1");
            return 1;
        }
        printf("%s %s\n", m.version, m.url);
        return 0;
    }
    if (!strcmp(argv[1], "folderline")) {
        /* folderline <name> <dir>: the status line for an app left out. */
        char line[96];
        catalog_folder_line(line, sizeof(line), argv[2], argv[3]);
        puts(line);
        return 0;
    }
    if (!strcmp(argv[1], "asset")) {
        /* asset <icon|shot|video> <id> <url> [cached]: what the media cache
           hands back, size and first bytes, fetched or kept. */
        enum asset_kind kind = !strcmp(argv[2], "icon") ? ASSET_ICON
                             : !strcmp(argv[2], "shot") ? ASSET_SHOT : ASSET_VIDEO;
        size_t len = 0;
        const unsigned char *b = asset_fetch(kind, argv[3], argv[4], argc > 5, &len);
        if (!b)
            return 1;
        printf("%lu %.*s\n", (unsigned long)len, (int)(len < 16 ? len : 16), (const char *)b);
        /* What the media thread does when it parks: the batch to the stick. */
        asset_flush();
        return 0;
    }
    if (!strcmp(argv[1], "categories")) {
        /* categories: each entry and the one group it stands in, "-" for
           none: what its page shows first and the store lists it under. */
        catalog_fetch(&catalog);
        for (int i = 0; i < catalog.count; i++)
            printf("%s %s\n", catalog.apps[i].id,
                   entry_category(&catalog.apps[i])[0] ? entry_category(&catalog.apps[i]) : "-");
        return 0;
    }
    if (!strcmp(argv[1], "listing")) {
        /* listing [category]: the Homebrew tab's package rows, top to
           bottom, by id; with a category, that category's rows. */
        catalog_fetch(&catalog);
        catalog_check_updates(&catalog);
        view_rebuild(&catalog);
        while (view_tab_kind() != VIEW_TAB_HOMEBREW) view_tab_move(1);
        if (getenv("UNRELEASED")) view_show_unreleased(1);
        for (int c = 0; argc > 2 && c < view_group_count(); c++)
            if (!strcmp(view_group_word(c), argv[2])) {
                view_group_open(c);
                break;
            }
        for (int row = 0; row < view_count(); row++) {
            int at = view_index(row);
            if (at >= 0) printf("%s\n", catalog.apps[at].id);
        }
        return 0;
    }
    if (!strcmp(argv[1], "groups")) {
        /* The Homebrew tab's first rows down to the first package, "way
           rows" each; then each way opened: "> way", its rows as kind, word
           and count, the row the first of them comes back to from inside,
           and the row the way comes back to. */
        catalog_fetch(&catalog);
        view_rebuild(&catalog);
        if (getenv("UNRELEASED")) view_show_unreleased(1);
        while (view_tab_kind() != VIEW_TAB_HOMEBREW) view_tab_move(1);
        int row = view_index(0) == VIEW_ROW_SEARCH;
        for (; row < view_count() && VIEW_IS_BROWSE(view_index(row)); row++)
            printf("%s %d\n", view_browse_word(VIEW_ROW_BROWSE - view_index(row)),
                   view_browse_rows(VIEW_ROW_BROWSE - view_index(row)));
        printf("then %d\n", view_index(row) >= 0);
        for (int k = 0; k < VIEW_BROWSE; k++) {
            view_browse_open(k);
            printf("> %s\n", view_browse_word(view_browse_at()));
            for (int r = 0; r < view_count(); r++) {
                int n = VIEW_ROW_GROUP - view_index(r);
                printf("%d %s %d\n", view_group_kind(n), view_group_word(n), view_group_apps(n));
            }
            if (view_count() > 1) {
                view_group_open(VIEW_ROW_GROUP - view_index(view_count() - 1));
                int apps = view_count();
                printf("apps %d back %d\n", apps, view_group_close());
            }
            printf("root %d\n", view_browse_close());
        }
        return 0;
    }
    if (!strcmp(argv[1], "cloud")) {
        /* cloud <room> <width>...: each chip's line and x, then where each
           goes left, right, up and down. */
        struct cloud_chip chip[64];
        float w[64];
        int n = argc - 3 < 64 ? argc - 3 : 64;
        for (int i = 0; i < n; i++) w[i] = (float)atof(argv[3 + i]);
        int lines = cloud_layout(chip, n, w, (float)atof(argv[2]), 6);
        printf("lines %d\n", lines);
        for (int i = 0; i < n; i++)
            printf("%d %g  %d %d %d %d\n", chip[i].line, chip[i].x, cloud_step(chip, n, i, -1, 0),
                   cloud_step(chip, n, i, 1, 0), cloud_step(chip, n, i, 0, -1),
                   cloud_step(chip, n, i, 0, 1));
        return 0;
    }
    if (!strcmp(argv[1], "sourcename")) {
        /* sourcename <url>...: what each source is called, one a line. */
        for (int i = 2; i < argc; i++) {
            char name[CATALOG_NAME_SIZE];
            sources_name(argv[i], name, sizeof(name));
            puts(name);
        }
        return 0;
    }
    if (!strcmp(argv[1], "view")) {
        /* The browser's model over the fetched catalog: which tabs there
           are, what each holds, the basket's tab coming and going, and what
           the caller is told when the tab under its cursor goes. */
        catalog_fetch(&catalog);
        catalog_check_updates(&catalog);
        view_rebuild(&catalog);
        int tabs = view_tab_count();
        printf("tabs %d:", tabs);
        for (int i = 0; i < tabs; i++) printf(" %d", view_tab_at(i));
        printf("\n");
        for (int i = 0; i < tabs; i++) {
            struct view_plan plan;
            view_action_plan(&plan);
            printf("tab %d kind %d rows %d first %d plan %d %d\n", view_tab_current(),
                   view_tab_kind(), view_count(), view_index(0),
                   plan.apps, plan.updates);
            view_tab_move(1);
        }
        unsigned gen = view_generation();
        view_basket_toggle(0);
        int kept = view_tabs_refresh();
        printf("basket %d kept %d tabs %d moved %d\n", view_basket_count(), kept,
               view_tab_count(), view_generation() != gen);
        while (view_tab_kind() != VIEW_TAB_BASKET) view_tab_move(1);
        printf("basket tab rows %d first %d index %d row %d\n", view_count(),
               view_index(0), view_index(1), view_row(0));
        gen = view_generation();
        view_basket_forget(0);
        kept = view_tabs_refresh();
        printf("emptied kept %d kind %d tabs %d moved %d\n", kept, view_tab_kind(),
               view_tab_count(), view_generation() != gen);
        return 0;
    }
    if (!strcmp(argv[1], "reach")) {
        /* What the gear's list of sources would say after a fetch: the
           apps, then each source and whether it answered. THEN_UP fetches
           once more with every host answering. */
        for (int round = 0; round < (getenv("THEN_UP") ? 2 : 1); round++) {
            if (round) {
                unsetenv("DOWN_HOST");
                printf("--\n");
            }
            catalog_fetch(&catalog);
            reach_take();
            struct sources s;
            sources_load(&s);
            for (int i = 0; i < catalog.count; i++)
                printf("%s\n", catalog.apps[i].id);
            for (int i = 0; i < s.count; i++)
                printf("%s %s\n", s.url[i],
                       reach_unreachable(s.url[i])    ? "unreachable"
                       : reach_offline_copy(s.url[i]) ? "offline copy"
                                                      : "ok");
        }
        return 0;
    }
    if (!strcmp(argv[1], "manage")) {
        /* Sources after a fetch: each row's name and status line,
           then what the right column says about it, the facts indented. */
        catalog_fetch(&catalog);
        reach_take();
        struct sources s;
        struct manage_sources m;
        sources_load(&s);
        memset(&m, 0, sizeof(m));
        manage_build(&m, &s);
        for (int i = 0; i < m.count; i++) {
            char note[256], facts[3][MANAGE_FACT];
            manage_note(&m, i, note, sizeof(note));
            printf("%s | %s | %s | %s\n", manage_title(&m, i), m.row[i].status,
                   manage_url(&m, i), note);
            int n = manage_facts(&m, i, facts, 3);
            for (int k = 0; k < n; k++) printf("  %s\n", facts[k]);
        }
        manage_move(&m, -1);
        printf("up from the top: %d\n", m.cursor);
        return 0;
    }
    if (!strcmp(argv[1], "presets")) {
        /* The start's merge alone: the program's list into sources.txt,
           or the list in presets-of-a-newer-build.txt where a test wrote
           one, for what a later release would bring. Never fatal. */
        FILE *f = fopen("presets-of-a-newer-build.txt", "rb");
        if (!f) {
            presets_merge();
            return 0;
        }
        static char list[4096];
        list[fread(list, 1, sizeof(list) - 1, f)] = 0;
        fclose(f);
        presets_offer(list);
        return 0;
    }
    if (!strcmp(argv[1], "add")) {
        char url[SOURCE_URL];
        if(sources_normalize(argv[2],url,sizeof(url))<0)return 1;
        if (catalog_validate_source(url, sources_kind(url) == SOURCE_REPO) < 0) {
            fprintf(stderr, "refused %d\n", catalog_refused(url));
            return 1;
        }
        return sources_add(url,url,sizeof(url))<0?1:0;
    }
    if (!strcmp(argv[1], "inboxinstall")) {
        if (getenv("FETCH_FIRST"))
            catalog_fetch(&catalog);
        int count=inbox_scan(&catalog);
        for(int i=0;i<count;i++){
            struct app_entry *entry=&catalog.apps[inbox_index(i)];struct install_report report;
            struct manifest m;memset(&m,0,sizeof(m));
            if(catalog_prepare(entry)==0 && entry_manifest(entry,&m)==0 && install_release(&m,&report,NULL,NULL,NULL)==0)inbox_installed(i);
            manifest_forget(&m);
        }return 0;
    }
    if (!strcmp(argv[1], "inbox")) {
        if (getenv("FETCH_FIRST"))
            catalog_fetch(&catalog);
        int n = inbox_scan(&catalog);
        printf("%d\n", n);
        /* The names the question lists, apart from the count tests read. */
        fprintf(stderr, "summary: %s\n", inbox_summary());
        return 0;
    }
    if (!strcmp(argv[1], "discard")) {
        char line[128];
        int rc = install_discard(line, sizeof(line));
        puts(line);
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "remove")) {
        if (argc > 2)
            host_fault(atol(argv[2]));
        return uninstall("io.github.test.demo") < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "uninstall")) {
        /* uninstall <id>: Delete on any row, and what it returned. */
        int rc = uninstall(argv[2]);
        printf("%d\n", rc);
        if (rc < 0 && plugin_refused()[0])
            fprintf(stderr, "why: %s\n", plugin_refused());
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "retire")) {
        /* The start's step before the self record: the one PSPDX 0.5 kept goes. */
        int rc = install_retire_legacy();
        printf("%d\n", rc);
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "install")) {
        struct manifest m = {0};
        struct pspdx_file spec;
        char why[80], *raw = NULL;
        int n = storage_read("manifest.json", &raw, PSPDX_FILE_MAX);
        if (n < 0 || pspdx_parse(raw, n, &spec, why, sizeof(why)) < 0)
            return 2;
        /* The text is the manifest's until the install is over. */
        m.raw = raw;
        struct source_repo repo;
        sources_parse_repo(spec.source, &repo);
        sources_repo_id(&repo, m.id, sizeof(m.id));
        strcpy(m.repo, spec.source);
        strcpy(m.dir, !strcmp(spec.type, "plugin") ? "" : spec.installdir[0] ? spec.installdir + 9 : "Demo");
        snprintf(m.url, sizeof(m.url), "%s/releases/download/v2/download.zip", spec.source);
        strcpy(m.version, getenv("VERSION") ? getenv("VERSION") : "2");
        m.rev = atoi(m.version);
        strcpy(m.checked_from, m.repo);
        m.checked_at = 123;
        SceIoStat st;
        sceIoGetstat(getenv("ZIP_FILE"), &st);
        m.size = st.st_size;
        if (getenv("BAD_HASH"))
            memset(m.sha256, 1, 32);
        struct install_report report;
        if (argc > 2)
            host_fault(atol(argv[2]));
        int rc = install_release_to(&m, getenv("INSTALL_DEVICE") ? getenv("INSTALL_DEVICE") : storage_device(),
                                    &report, test_phase, test_progress, NULL);
        manifest_forget(&m);
        printf("%d %u %llu\n", rc, host_operations(), report.needed);
        if (rc < 0 && report.why[0])
            fprintf(stderr, "why: %s\n", report.why);
        if (rc == 0 && report.plugin[0])
            fprintf(stderr, "plugin: %s %s\n", report.plugin, report.plugin_off ? "off" : "on");
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "get")) {
        /* What X on a row does once it is answered: the entry with this id
           prepared and installed, and the record it writes left behind. */
        catalog_fetch(&catalog);
        for (int i = 0; i < catalog.count; i++)
            if (!strcmp(catalog.apps[i].id, argv[2])) {
                struct install_report report;
                int rc = catalog_prepare(&catalog.apps[i]);
                struct manifest m;
                memset(&m, 0, sizeof(m));
                if (rc == 0)
                    rc = entry_manifest(&catalog.apps[i], &m) == 0
                             ? install_release(&m, &report, NULL, NULL, NULL) : -1;
                manifest_forget(&m);
                printf("%d\n", rc);
                if (rc < 0 && report.why[0])
                    fprintf(stderr, "why: %s\n", report.why);
                return rc < 0 ? 1 : 0;
            }
        return 2;
    }
    if (!strcmp(argv[1], "prepare")) {
        /* The step before every install, for the entry with this id. */
        catalog_fetch(&catalog);
        for (int i = 0; i < catalog.count; i++)
            if (!strcmp(catalog.apps[i].id, argv[2])) {
                int rc = catalog_prepare(&catalog.apps[i]);
                printf("%d\n", rc);
                return rc < 0 ? 1 : 0;
            }
        return 2;
    }
    return 2;
}
