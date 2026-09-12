#include "install/state.h"
#include "pspiofilemgr.h"
#include "update/catalog.h"
#include "update/inbox.h"
#include "update/sources.h"
#include "util/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void host_fault(long);
unsigned host_operations(void);
static struct catalog catalog;
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    storage_init(getenv("DEVICE") ? getenv("DEVICE") : "ms0:/PSP/GAME/PSPDX/EBOOT.PBP");
    host_fault(getenv("LOAD_FAULT") ? atol(getenv("LOAD_FAULT")) : 0);
    state_load();
    host_fault(0);
    if (!strcmp(argv[1], "parse")) {
        char *raw = NULL;
        struct pspdx_file f;
        char why[100];
        int n = storage_read(argv[2], &raw, PSPDX_FILE_MAX);
        int rc = n < 0 ? -1 : pspdx_parse(raw, n, &f, why, sizeof(why));
        free(raw);
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "recover")) {
        host_fault(argc > 2 ? atol(argv[2]) : 0);
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
            printf("%s %s %d %d\n", catalog.apps[i].id, catalog.apps[i].release.version,
                   catalog.apps[i].fresh, catalog.apps[i].state);
        return rc < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "add")) {
        char url[SOURCE_URL];
        if(sources_normalize(argv[2],url,sizeof(url))<0)return 1;
        if(catalog_validate_source(url,sources_kind(url)==SOURCE_REPO)<0)return 1;
        return sources_add(url,url,sizeof(url))<0?1:0;
    }
    if (!strcmp(argv[1], "inboxinstall")) {
        int count=inbox_scan(&catalog);
        for(int i=0;i<count;i++){
            struct app_entry *entry=&catalog.apps[inbox_index(i)];struct install_report report;
            if(catalog_prepare(entry)==0 && install_release(&entry->release,&report,NULL,NULL,NULL)==0)inbox_installed(i);
        }return 0;
    }
    if (!strcmp(argv[1], "inbox")) {
        int n = inbox_scan(&catalog);
        printf("%d\n", n);
        return 0;
    }
    if (!strcmp(argv[1], "remove")) {
        host_fault(argc > 2 ? atol(argv[2]) : 0);
        return uninstall("io.github.test.demo") < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "install")) {
        struct manifest m = {0};
        struct pspdx_file spec;
        char why[80], *raw = NULL;
        int n = storage_read("manifest.json", &raw, PSPDX_FILE_MAX);
        if (n < 0 || pspdx_parse(raw, n, &spec, why, sizeof(why)) < 0)
            return 2;
        memcpy(m.raw, raw, n + 1);
        free(raw);
        struct source_repo repo;
        sources_parse_repo(spec.source, &repo);
        sources_repo_id(&repo, m.id, sizeof(m.id));
        strcpy(m.repo, spec.source);
        strcpy(m.dir, spec.installdir + 9);
        strcpy(m.url, "https://github.com/test/demo/releases/download/v2/download.zip");
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
        host_fault(argc > 2 ? atol(argv[2]) : 0);
        int rc = install_release(&m, &report, NULL, NULL, NULL);
        printf("%d %u\n", rc, host_operations());
        return rc < 0 ? 1 : 0;
    }
    return 2;
}
