#ifndef PSPDX_TEXT_H
#define PSPDX_TEXT_H

/* Every word PSPDX puts on the screen, in one place, and in the voice of the
   system it runs on: short sentences in the system's own phrasing -- "Do you
   want to ...?", "Could not ...", "... was installed." -- and none of the
   paths, codes or internals nobody holding the console has a use for. The
   log keeps those.

   Some of these are printf formats; what each takes is in the comment. */

/* ---------------------------------------------------------------- header */

#define T_TAB_ALL            "All"
#define T_TAB_GAMES          "Games"
#define T_TAB_DEMOS          "Demos"
#define T_TAB_APPS           "Apps"
#define T_TAB_EMULATORS      "Emulators"
#define T_TAB_PLUGINS        "Plugins"
#define T_HEAD_GEAR          "PSPDX"
#define T_HEAD_STICK         "Installed"
#define T_HEAD_BASKET        "Basket"
#define T_HEAD_FILES         "Manage Data"

/* ------------------------------------------------------ waiting and status */

#define T_WORD_CONNECTING    "Connecting"
#define T_WORD_CHECKING      "Checking"
#define T_WORD_INBOX         "Checking INBOX"
#define T_WORD_OFFLINE       "Offline"
#define T_STATUS_CONNECTING  "Connecting..."
#define T_STATUS_LOADING     "Loading catalog..."
#define T_STATUS_CHECKING    "Checking for updates..."
#define T_STATUS_ORIGIN      "Checking %.28s (%d of %d)..."       /* repository, at, of */
#define T_STALE              "%s has not been updated for %u day%s." /* host, days, "s" */
#define T_CATALOG_EMPTY      "No apps are available."

/* ---------------------------------------------------------------- the list */

#define T_DOWNLOAD_ALL       "Download all"
#define T_UPDATE_ALL         "Update all"
#define T_CHECK              "Check for updates"
#define T_ALL_CURRENT        "All up to date"
#define T_NO_RELEASES        "No releases available"
#define T_PLAN_LINE          "%d app%s, %s"                       /* n, "s", size */
#define T_PLAN_SIZE          "%s, about %s"                       /* size, time */
#define T_PLAN_SKIPPED       "%d without a release"               /* n */
#define T_AND_MORE           "and %d more"                        /* n */

/* The check row's panel: two keys, and what each of them does. */
#define T_CHECK_NOTE         "Finds newer versions of your apps."
#define T_QUICK_CHECK        "Quick check"
#define T_FULL_CHECK         "Full check"
#define T_CHECK_EXPLAIN      "Quick check reads the catalog. Full check asks GitHub about each app and takes longer."

/* The card under the picture. */
#define T_PANEL_UPDATE       "Update to %s   %s"                  /* version, size */
#define T_PANEL_INSTALLED    "Version %s"                         /* version */
#define T_PANEL_NO_RELEASE   "No release"
#define T_PANEL_IN_BASKET    "   In basket"

/* ---------------------------------------------------------------- options */

#define T_MENU_RUN           "Run"
#define T_MENU_RESTART       "Restart"
#define T_MENU_INSTALL       "Install"
#define T_MENU_REINSTALL     "Reinstall"
#define T_MENU_UPDATE        "Update to %.20s"                    /* version */
#define T_MENU_DELETE        "Delete"
#define T_MENU_BASKET_IN     "Add to"                             /* then the basket mark */
#define T_MENU_BASKET_OUT    "Remove from"                        /* then the basket mark */
#define T_MENU_INFO          "Information"

#define T_HINT_ENTER         "Enter"
#define T_HINT_BACK          "Back"
#define T_HINT_CANCEL        "Cancel"

/* --------------------------------------------------------------- questions */

#define T_INSTALL_ASK        "Do you want to install %s?"         /* name */
#define T_UPDATE_ASK         "Do you want to update %s?"          /* name */
#define T_INSTALL_LINE       "Version %s, %lu.%lu MB"             /* version, MB, tenths */
#define T_INSTALL_LINE_NOSIZE "Version %s"                        /* version */
#define T_INSTALL_MOVES      "It moves to the folder %.32s."      /* folder */
#define T_DIR_OTHER_APP      "Another app uses the folder %.32s." /* folder */
#define T_DIR_BAK_EXISTS     "Could not install. %.32s.bak already exists." /* folder */
#define T_DIR_EXISTS_ASK     "The folder %.32s already exists."   /* folder */
#define T_DIR_EXISTS_LINE    "Do you want to rename it to %.32s.bak and install?" /* folder */
#define T_RENAME_FAILED      "Could not rename the folder %.32s." /* folder */
#define T_REMOVE_ASK         "Do you want to delete %s?"          /* name */
#define T_REMOVE_LINE        "Save data is not deleted."
#define T_ALL_ASK_INSTALL    "Do you want to install %d app%s?"   /* n, "s" */
#define T_ALL_ASK_UPDATE     "Do you want to update %d app%s?"    /* n, "s" */
#define T_ALL_SIZE           "%s in total"                        /* size */
#define T_ALL_AGAIN          ", %d again"                         /* n */
#define T_ALL_SKIPPED        ", %d skipped"                       /* n */
#define T_RUN_ASK            "Do you want to start %.40s?"        /* name */
#define T_RUN_LINE           "PSPDX will close."
#define T_RESTART_ASK        "Do you want to restart PSPDX now?"
#define T_RESTART_LINE       "Version %.20s takes effect after restarting." /* version */
#define T_RESTART_LATER      "Restart PSPDX to finish the update."
#define T_INBOX_ASK          "Do you want to install %d apps from INBOX?" /* n */
#define T_SOURCE_DELETE_ASK  "Do you want to delete this source?"
#define T_RESET_ASK          "Do you want to reset PSPDX?"
#define T_RESET_LINE         "All PSPDX data is deleted. Installed apps are not deleted."
#define T_DISCARD_ASK        "Do you want to clean up the failed install?"
#define T_DISCARD_LINE       "Leftover download data is deleted. Installed apps are not changed."

/* ---------------------------------------------------------------- results */

#define T_INSTALLED          "%s %s was installed."               /* name, version */
#define T_UPDATED_SELF       "PSPDX was updated to %s."           /* version */
#define T_CANCELLED          "Installation of %s was canceled."   /* name */
#define T_INSTALL_FAILED     "Could not install %s (%d)."         /* name, code */
#define T_REMOVED            "%s was deleted."                    /* name */
#define T_REMOVE_FAILED      "Could not delete %s (%d)."          /* name, code */
#define T_ALL_DONE           "%d of %d apps were installed."      /* done, n */
#define T_INBOX_DONE         "%d of %d apps were installed from INBOX." /* done, n */
#define T_INBOX_EMPTY        "No apps were found in INBOX."
#define T_NOTHING_TO_DOWNLOAD "There is nothing to download."
#define T_NO_RECORD          "The install location of this app is unknown."
#define T_NO_EBOOT           "This app cannot be started."
#define T_START_REFUSED      "Could not start the app."
#define T_SELF_DELETE        "PSPDX cannot delete itself."
#define T_DISCARD_FAILED     "Could not clean up the failed install."

/* Direct install and sources. */
#define T_OSK_GITHUB         "Enter GitHub owner/repository"
#define T_OSK_SOURCE         "Enter source URL"
#define T_BAD_ADDRESS        "The address is not valid."
#define T_GITHUB_FORMAT      "Enter it as owner/repository."
#define T_SOURCE_UNAVAILABLE "Could not add the source."
#define T_SOURCE_SAVE_FAILED "Could not save the source."
#define T_SOURCE_EXISTS      "This source has already been added."
#define T_SOURCE_DELETE_FAILED "Could not delete the source."
#define T_WANT_NO_PSPDX      "%.62s has no .pspdx file."          /* owner/repo */
#define T_WANT_NO_REPO       "%.62s was not found on GitHub."     /* owner/repo */
#define T_WANT_NO_RELEASE    "%.62s has no release to install."   /* owner/repo */
#define T_WANT_FAILED        "Could not add %.60s."               /* owner/repo */
#define T_WANT_CURRENT       "%s is up to date."                  /* name */

/* ------------------------------------------------------------------ gear */

#define T_SET_SOURCES        "Add sources"
#define T_SET_DIRECT         "Direct install"
#define T_SET_FILES          "Manage Data"
#define T_SET_RESET          "Reset"
#define T_SET_INFO           "Information"
#define T_NOTE_SOURCES       "Add or delete the catalogs PSPDX reads apps from."
#define T_NOTE_DIRECT        "Install an app from GitHub or from the INBOX folder."
#define T_NOTE_FILES         "View the data PSPDX keeps on the Memory Stick."
#define T_NOTE_RESET         "Reset PSPDX, clean up after a failed install, or renew the TLS seed."
#define T_NOTE_INFO          "View details about PSPDX and this system."

#define T_SUB_SOURCES        "Sources"
#define T_SUB_DIRECT         "Direct install"
#define T_SUB_RESET          "Reset"
#define T_SUB_ADD_SOURCE     "Add source"
#define T_SUB_FROM_GITHUB    "From GitHub"
#define T_SUB_FROM_INBOX     "From INBOX folder"
#define T_SUB_SWEEP          "Renew TLS Seed"
#define T_SUB_RESET_ALL      "Reset PSPDX"
#define T_SUB_DISCARD        "Clean Up Failed Installs"

/* ------------------------------------------------------------ information */

#define T_INFO_PSPDX         "PSPDX"
#define T_INFO_CATALOG       "Catalog"
#define T_INFO_CONNECTION    "Connection"
#define T_INFO_HANDSHAKE     "Handshake"
#define T_INFO_ENTROPY       "Entropy"
#define T_INFO_FRAMES        "Frames"
#define T_INFO_INSTALLED     "Installed"
#define T_INFO_MEMORY        "Memory free"
#define T_INFO_STICK         "Stick free"

#define T_DETAIL_VERSION     "Version"
#define T_DETAIL_AUTHOR      "Author"
#define T_DETAIL_LICENSE     "License"
#define T_DETAIL_CATEGORY    "Category"
#define T_DETAIL_SIZE        "Size"
#define T_DETAIL_ID          "ID"
#define T_DETAIL_CHECKED     "Last checked"
#define T_DETAIL_UPDATE      "%.31s, %.31s available"             /* installed, published */
#define T_DETAIL_INSTALLED   "%s (installed)"                     /* version */
#define T_DETAIL_SAVED       "Saved "
#define T_DETAIL_THIS_SESSION "This session"
#define T_UNKNOWN            "Unknown"

/* ------------------------------------------------------------------ files */

#define T_FILES_EMPTY        "Nothing here."
#define T_FILES_BINARY       "This file cannot be displayed."
#define T_FILES_BYTES        "%u bytes"                           /* size */
#define T_FILES_KB           "%u KB"                              /* size */
#define T_FILES_MB           "%u.%u MB"                           /* MB, tenths */
#define T_FILES_N            "%d"                                 /* count */
#define T_FILES_NONE         "none"
#define T_HINT_OPEN          "Open"
#define T_HINT_SCROLL        "Scroll"
#define T_HINT_OPEN_RAW      "Open"
#define T_HINT_STICK_SCROLL  "Scroll"

/* The areas, in the order a person asks about them. */
#define T_AREA_SYSTEM        "System"
#define T_AREA_SYSTEM_NOTE   "Cache, logs and other files PSPDX keeps for itself."
#define T_AREA_SOURCES       "Sources"
#define T_AREA_INSTALLED     "Installed apps"
#define T_AREA_INBOX         "Inbox"
#define T_AREA_CACHE         "Cache"
#define T_AREA_PENDING       "Unfinished install"
#define T_AREA_LOGS          "Logs"
#define T_AREA_SEED          "Encryption seed"
#define T_AREA_DEBUG         "Developer files"
#define T_AREA_SOURCES_NOTE  "Where PSPDX looks for apps. Edit under Add sources."
#define T_AREA_INSTALLED_NOTE "What is installed and where it came from. One entry per app."
#define T_AREA_INBOX_NOTE    ".pspdx files copied here are installed via Direct install."
#define T_AREA_CACHE_NOTE    "Catalogs and previews already downloaded. Safe to delete."
#define T_AREA_PENDING_NOTE  "Used only while an install is running. Empty means nothing is stuck."
#define T_AREA_LOGS_NOTE     "What PSPDX did, for troubleshooting."
#define T_AREA_SEED_NOTE     "The random seed for secure connections. Never shown."
#define T_AREA_DEBUG_NOTE    "Screenshots and test files for development."
#define T_KIND_CATALOG       "Catalog"
#define T_CACHE_ICON         "Icon of %s"                         /* app */
#define T_CACHE_PICTURE      "Picture of %s"                      /* app */
#define T_CACHE_FILM         "Video of %s"                        /* app */
#define T_CACHE_SOUND        "Sound of %s"                        /* app */
#define T_CACHE_CATALOG      "Catalog"
#define T_KIND_LIST          "Repository list"
#define T_KIND_REPO          "Repository"
#define T_APP_INSTALLED      "Installed: %s"                      /* version */
#define T_APP_LATEST         "Latest: %s"                         /* version */
#define T_APP_FOLDER         "Folder: PSP/GAME/%s"                /* dir */
#define T_APP_SOURCE         "Source: %s"                         /* url */
#define T_APP_CHECKED        "Checked: %s"                        /* when */
#define T_APP_VIA            "Via: %s"                            /* host */
#define T_APP_FILES_NOTE     "The two files PSPDX keeps for this app."
#define T_FILE_NOTE          "%s in %s"                           /* size, area */

#endif
