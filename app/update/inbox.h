#ifndef PSPDX_INBOX_H
#define PSPDX_INBOX_H
#include "update/catalog.h"
int inbox_scan(struct catalog *catalog);
int inbox_count(void);
int inbox_index(int n);
void inbox_installed(int n);
const char *inbox_summary(void);
#endif
