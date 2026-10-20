#ifndef PSPDX_NETCONF_H
#define PSPDX_NETCONF_H
/* Main thread, with network/media workers stopped. 0 connected, -1 offline. */
int netconf_connect(void);
/* Last attempt was refused or ended without loading Sony's dialog plugin. */
int netconf_failed_to_open(void);
#endif
