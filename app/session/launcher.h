#ifndef PSPDX_LAUNCHER_H
#define PSPDX_LAUNCHER_H

/* PSPDX as ARK's custom launcher: started by the custom firmware in the home
   menu's place. ARK starts whatever is VBOOT.PBP in its own folder,
   PSP/SAVEDATA/ARK_01234, while its SETTINGS.TXT has the launcher on; both
   are set and taken back here. Nothing here starts or stops anything: the
   way out to the home menu is session/actions.c's leave_to_xmb(). */

/* Whether ARK has the launcher on: SETTINGS.TXT read at the first ask, and
   what launcher_set made of it after that. 0 without ARK. */
int launcher_mode_on(void);

/* What launcher_set could not do. */
enum launcher_refusal {
    LAUNCHER_NO_ARK = -1,       /* no ARK_01234 folder on either device */
    LAUNCHER_NO_SETTINGS = -2,  /* the folder is there, its SETTINGS.TXT is not */
    LAUNCHER_NO_SELF = -3,      /* PSPDX's own EBOOT.PBP is not where it was started from */
    LAUNCHER_FAILED = -4,       /* a copy or a write on the stick failed */
};

/* On: ARK's VBOOT.PBP put aside once as VBOOT_ARK.PBP, PSPDX's own EBOOT.PBP
   copied in its place, and the launcher line set. Off: the line set off and
   ARK's VBOOT.PBP put back, or PSPDX's taken away when there was none.
   0 when done, a launcher_refusal when not; nothing is half done then. */
int launcher_set(int on);

/* For the way out to the home menu with the launcher on, which would
   otherwise start PSPDX again: the line set off, and a mark left that
   launcher_resume() reads. 0 when the line is off. */
int launcher_suspend(void);

/* At the start, before launcher_mode_on() is first asked: when the last run
   left for the home menu with the launcher suspended, the mark goes and the
   line is set on again; and ARK's copy of PSPDX is brought up to the
   PSPDX in PSP/GAME when an update has changed that. */
void launcher_resume(void);

/* Whether this run was started by ARK as its launcher, from argv[0]: the
   start then is the library's, with no network. Told once, first thing. */
void launcher_note_start(const char *argv0);
int launcher_started(void);

#endif
