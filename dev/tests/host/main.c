#include "install/state.h"
#include "pspiofilemgr.h"
#include "session/view.h"
#include "update/catalog.h"
#include "update/inbox.h"
#include "update/presets.h"
#include "update/reach.h"
#include "update/sources.h"
#include "gui/wrap.h"
#include "util/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void host_fault(long);
unsigned host_operations(void);
static struct catalog catalog;
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
            printf("%s %s %d %d %d\n", catalog.apps[i].id, catalog.apps[i].release.version,
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
                printf("%s\n%s\n%s\n%s\n", catalog.apps[i].icon, catalog.apps[i].screenshot,
                       catalog.apps[i].video, catalog.apps[i].sound);
                return 0;
            }
        return 2;
    }
    if (!strcmp(argv[1], "release")) {
        /* release <id>: the version, the zip and whether the file pinned it. */
        catalog_fetch(&catalog);
        for (int i = 0; i < catalog.count; i++)
            if (!strcmp(catalog.apps[i].id, argv[2])) {
                printf("%s %s %d\n", catalog.apps[i].release.version, catalog.apps[i].release.url,
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
                const struct manifest *m = &catalog.apps[i].release;
                if (manifest_has_sha256(m))
                    for (int k = 0; k < 32; k++) printf("%02x", m->sha256[k]);
                else
                    printf("-");
                printf(" %s\n", m->url);
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
    if (!strcmp(argv[1], "presets")) {
        /* The start's merge alone: the list beside the EBOOT the stick
           was booted from, into sources.txt. Never fatal. */
        presets_merge(getenv("DEVICE") ? getenv("DEVICE") : "ms0:/PSP/GAME/PSPDX/EBOOT.PBP");
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
            if(catalog_prepare(entry)==0 && install_release(&entry->release,&report,NULL,NULL,NULL)==0)inbox_installed(i);
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
        strcpy(m.dir, spec.installdir[0] ? spec.installdir + 9 : "Demo");
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
        int rc = install_release(&m, &report, NULL, NULL, NULL);
        manifest_forget(&m);
        printf("%d %u %llu\n", rc, host_operations(), report.needed);
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
                if (rc == 0)
                    rc = install_release(&catalog.apps[i].release, &report, NULL, NULL, NULL);
                printf("%d\n", rc);
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
