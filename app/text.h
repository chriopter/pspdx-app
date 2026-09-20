#ifndef PSPDX_TEXT_H
#define PSPDX_TEXT_H

/* Every word PSPDX puts on the screen, in one place, and in the voice of the
   system it runs on: short sentences in the system's own phrasing -- "Do you
   want to ...?", "Could not ...", "... was installed." -- and none of the
   paths, codes or internals nobody holding the console has a use for. The
   log keeps those.

   Some of these are printf formats; what each takes is in the comment. */

/* ---------------------------------------------------------------- header */

#define T_HEAD_GEAR          "PSPDX"
#define T_HEAD_STICK         "Installed"
#define T_HEAD_BASKET        "Basket"
#define T_HEAD_HOMEBREW      "Homebrews"
#define T_HEAD_UMD           "UMDs"
#define T_HEAD_FILES         "Data"
#define T_HEAD_SYSTEM        "Options"
#define T_HEAD_ABOUT         "About"
#define T_HEAD_SOURCES       "Sources"
#define T_UMD_SOON           "Coming soon"

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
#define T_STATUS_UNREACHABLE "Could not load the catalog."
#define T_STATUS_TOO_LARGE   "The catalog is too large."
#define T_STATUS_OFFLINE     "Offline: last known releases"
#define T_STATUS_RETRY       "%s   X to try again"                 /* what went wrong */
#define T_STATUS_BENCH       "Benchmarking ciphers..."

/* ---------------------------------------------------------------- the list */

#define T_DOWNLOAD_ALL       "Download all"
#define T_UPDATES            "%d update%s"                        /* n, "s": the stick's job row */
#define T_NO_UPDATES         "No updates"
#define T_ALL_CURRENT        "Up to date"
#define T_NO_RELEASES        "No releases available"
#define T_PLAN_LINE          "%d app%s, %s"                       /* n, "s", size */
#define T_PLAN_SIZE          "%s, about %s"                       /* size, time */
#define T_PLAN_SKIPPED       "%d without a release"               /* n */
#define T_AND_MORE           "and %d more"                        /* n */

/* The check row's panel: two keys, and what each of them does. */
#define T_CHECK_NOTE         "Checks for updates."
#define T_QUICK_CHECK        "Quick check"
#define T_FULL_CHECK         "Full check"
#define T_CHECK_EXPLAIN      "Quick checks the catalog. Full checks each app on GitHub."

/* The card under the picture. */
#define T_PANEL_UPDATE       "Update to %s   %s"                  /* version, size */
#define T_PANEL_REBUILD      "Update to %s (new build)   %s"      /* version, size */
#define T_PANEL_INSTALLED    "Version %s"                         /* version */
#define T_PANEL_NO_RELEASE   "No release"
#define T_PANEL_UNSUPPORTED  "Not installable yet"
#define T_PANEL_IN_BASKET    "   In basket"

/* On the card, in place of a picture. */
#define T_CARD_LOADING       "loading"
#define T_CARD_NO_PICTURE    "no picture"

/* ---------------------------------------------------------------- options */

#define T_MENU_RUN           "Run"
#define T_MENU_RESTART       "Restart"
#define T_MENU_INSTALL       "Install"
#define T_MENU_REINSTALL     "Reinstall"
#define T_MENU_UPDATE        "Update to %s"                       /* version, at most 20 bytes */
#define T_MENU_REBUILD       "Update (new build)"
#define T_MENU_DELETE        "Delete"
#define T_MENU_BASKET_IN     "Add to"                             /* then the basket mark */
#define T_MENU_BASKET_OUT    "Remove from"                        /* then the basket mark */
#define T_MENU_INFO          "Information"

#define T_HINT_ENTER         "Enter"
#define T_HINT_OPTIONS       "Options"
#define T_HINT_UPDATE        "Update"
#define T_HINT_BACK          "Back"
#define T_HINT_CANCEL        "Cancel"
#define T_HINT_DELETE        "Delete"

/* --------------------------------------------------------------- questions */

#define T_INSTALL_ASK        "Do you want to install %s?"         /* name */
#define T_UPDATE_ASK         "Do you want to update %s?"          /* name */
#define T_INSTALL_LINE       "Version %s, %lu.%lu MB"             /* version, MB, tenths */
#define T_INSTALL_LINE_REBUILD "Version %s (new build), %lu.%lu MB" /* version, MB, tenths */
#define T_INSTALL_LINE_NOSIZE "Version %s"                        /* version */
#define T_INSTALL_MOVES      " Its folder changes from %.32s to %.32s."   /* old, new; after the version line */
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
#define T_RESTART_LINE       "Version %s takes effect after restarting." /* version, at most 20 bytes */
#define T_RESTART_LATER      "Restart PSPDX to finish the update."
#define T_INBOX_ASK          "Do you want to install %d apps from INBOX?" /* n */
#define T_SOURCE_DELETE_ASK  "Do you want to delete this source?"
#define T_TRUST_ASK          "Do you want to connect anyway?"
#define T_TRUST_EXPIRED      "The security certificate of %s has expired. The connection cannot be verified."
#define T_TRUST_ISSUER       "The security certificate of %s is not from a known issuer. The connection cannot be verified."
#define T_TRUST_DECLINED     "Not connected."
#define T_RESET_ASK          "Do you want to restore the default settings?"
#define T_RESET_LINE         "All PSPDX data is deleted. Installed apps are not deleted."
#define T_DISCARD_ASK        "Do you want to clear the cache?"
#define T_DISCARD_LINE       "Downloaded catalogs, previews and any failed install are deleted. Installed apps are not changed."

#define T_YES                "Yes"
#define T_NO                 "No"

/* ---------------------------------------------------------------- results */

#define T_INSTALLED          "%s %s was installed."               /* name, version */
#define T_INSTALLED_NO_PSPDX "%s %s was installed. Its repository has no .pspdx." /* name, version */
#define T_INSTALLED_FROM_FILE "%s %s was installed from your file; the repository has no .pspdx." /* name, version */
#define T_UPDATED_SELF       "PSPDX was updated to %s."           /* version */
#define T_CANCELLED          "Installation of %s was canceled."   /* name */
#define T_INSTALL_FAILED     "Could not install %s (%d)."         /* name, code */
#define T_NO_SPACE           "Could not install %s. %lu.%lu MB of free space is needed." /* name, MB, tenths */
#define T_INSTALL_UNSUPPORTED "%.60s cannot be installed yet."   /* name */
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

/* Direct Install and sources. */
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
#define T_WANT_NO_ANSWER     "GitHub did not answer for %.50s (rate limit or error)." /* owner/repo */
#define T_FOLDER_TAKEN       "%s not listed: PSP/GAME/%s is another app's." /* name or owner/repo, folder */
#define T_WANT_CURRENT       "%s is up to date."                  /* name */

/* ------------------------------------------------------------------ gear */

/* The four rows under the gear, the way the system names its own: a noun
   each, and on the right what taking the row comes to. */
#define T_SET_SOURCES        "Sources"
#define T_SET_SYSTEM         "Options"
#define T_SET_FILES          "Data"
#define T_SET_ABOUT          "About"
#define T_SET_DIRECT         "Direct Install"
#define T_NOTE_SOURCES       "Manages the catalogs apps are listed from."
#define T_NOTE_SYSTEM        "Adjusts display, testing and default settings."
#define T_NOTE_FILES         "Displays the data saved on the Memory Stick."
#define T_NOTE_ABOUT         "Displays version and system information."
#define T_NOTE_DIRECT        "Installs an app from GitHub or from the INBOX folder."

#define T_SUB_DIRECT         "Direct Install"
#define T_SUB_FROM_GITHUB    "From GitHub"
#define T_SUB_FROM_INBOX     "From INBOX Folder"
/* Options: a value row first, the value at the row's end; then the
   switches, the tick mark after each while it is on; the seed's renewal;
   and last the one that takes everything back. On the right, what each
   comes to. */
#define T_SYS_BAKED          "Baked UI"                        /* the row's word at 30 FPS */
#define T_SYS_MERCY          "Mercy UI"                        /* the row's word at 60 FPS */
#define T_VALUE_FPS30        "30 FPS"
#define T_VALUE_FPS60        "60 FPS"
#define T_SYS_FPS            "Show FPS"
#define T_SYS_DEV            "Fake Updates"                      /* every installed package is said to have an update */
#define T_SYS_SWEEP          "Renew TLS Seed"
#define T_SYS_RESET_ALL      "Restore Defaults"
#define T_SYS_FRAME_RATE_NOTE "Switches between baked UI at 30 FPS and performance UI at 60 FPS."
#define T_SYS_FPS_NOTE       "Displays the frame rate on screen."
#define T_SYS_DEV_NOTE       "Testing option to simulate updates for installed apps."
#define T_SYS_SWEEP_NOTE     "Generates a new seed for secure connections."
#define T_SYS_RESET_NOTE     "Restores all settings to their defaults. Installed apps are not deleted."
#define T_HINT_CHANGE        "Change"
#define T_HINT_TOGGLE        "Toggle"
#define T_HINT_RUN           "Run"
#define T_HINT_RESTORE       "Restore"
#define T_HINT_UI_MODE       "UI Mode"                          /* SELECT: the frame rate for this run */
#define T_HINT_TABS          "Tabs"
/* Data: the one action at the top of the areas. */
#define T_SUB_CLEAR_CACHE    "Clear Cache"
#define T_CACHE_CLEARED      "Cache cleared."
#define T_CACHE_CLEAR_FAILED "Could not clear the cache."

/* Sources: Add Source at the top, then a row per source with how the
   last fetch went under its name; on the right what the row is. */
#define T_SUB_ADD_SOURCE     "Add Source"
#define T_SOURCE_APPS        "%d app%s"                           /* n, "s" */
#define T_SOURCE_OFFLINE     "offline copy, %d app%s"             /* n, "s"; after the kind */
#define T_SOURCE_UNREACHABLE "unreachable"
#define T_SOURCE_NOT_LOADED  "not loaded yet"
#define T_SOURCE_KIND_SITE   "Catalog site"
#define T_SOURCE_KIND_JSON   "catalog.json"
#define T_SOURCE_KIND_LIST   "Text list"
#define T_SOURCE_KIND_REPO   "Repository"
#define T_SOURCE_FACT_KIND   "Kind: %s"                           /* kind */
#define T_SOURCE_FACT_APPS   "Apps: %d"                           /* n */
#define T_SOURCE_FACT_LOADED "Loaded: %s"                         /* when */
#define T_SOURCE_FACT_SAVED  "Loaded: saved copy"
#define T_SOURCE_NOTE_ADD    "Adds a catalog site, a catalog.json or a text list."
#define T_SOURCE_NOTE_SITE   "Catalog site."
#define T_SOURCE_NOTE_JSON   "Catalog file."
#define T_SOURCE_NOTE_LIST   "List of GitHub repositories."
#define T_SOURCE_NOTE_REPO   "GitHub repository."
#define T_SOURCE_NOTE_UNREACHABLE "Could not be loaded. Its apps are not displayed."
#define T_SOURCE_NOTE_OFFLINE "Could not be loaded. The saved copy is displayed."
#define T_SOURCE_NOTE_KEEP   "Deleting it does not delete installed apps."

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
#define T_INFO_TLS           "TLS 1.3  %s  %s"                    /* cipher, group */
#define T_INFO_NOT_CONNECTED "not connected"
#define T_INFO_UNKNOWN       "unknown"

#define T_DETAIL_VERSION     "Version"
#define T_DETAIL_AUTHOR      "Author"
#define T_DETAIL_LICENSE     "License"
#define T_DETAIL_TAGS        "Tags"
#define T_DETAIL_CATEGORY    "Category"
#define T_CATEGORY_NOTE      "%d app%s in this category: "   /* n, "s"; the names follow */
#define T_DETAIL_SIZE        "Size"
#define T_DETAIL_ID          "ID"
#define T_DETAIL_CHECKED     "Last checked"
#define T_DETAIL_UPDATE      "%s, %s available"                   /* installed, published */
#define T_DETAIL_REBUILD     "%s, new build available"            /* installed */
#define T_DETAIL_INSTALLED   "%s (installed)"                     /* version */
#define T_DETAIL_SAVED       "Saved "
#define T_DETAIL_THIS_SESSION "This session"
#define T_UNKNOWN            "Unknown"

/* ------------------------------------------------------------------ files */

#define T_FILES_EMPTY        "Nothing here."
#define T_FILES_BINARY       "This file cannot be displayed."
#define T_FILES_LOADING      "Loading..."
#define T_FILES_PLAYING      "Now playing."
#define T_FILES_BYTES        "%u bytes"                           /* size */
#define T_FILES_KB           "%u KB"                              /* size */
#define T_FILES_MB           "%u.%u MB"                           /* MB, tenths */
#define T_FILES_N            "%d"                                 /* count */
#define T_FILES_NONE         "none"
#define T_HINT_OPEN          "Open"
#define T_HINT_CLEAR         "Clear"
#define T_HINT_SCROLL        "Scroll"
#define T_HINT_OPEN_RAW      "Open"
#define T_HINT_STICK_SCROLL  "Scroll"

/* The areas, in the order a person asks about them. */
#define T_AREA_SYSTEM        "System"
#define T_AREA_SYSTEM_NOTE   "Cache, logs and other system files."
#define T_AREA_SOURCES       "Sources"
#define T_AREA_INSTALLED     "Installed apps"
#define T_AREA_INBOX         "Inbox"
#define T_AREA_CACHE         "Cache"
#define T_AREA_PENDING       "Unfinished install"
#define T_AREA_LOGS          "Logs"
#define T_AREA_SEED          "Encryption seed"
#define T_AREA_DEBUG         "Developer files"
#define T_AREA_SOURCES_NOTE  "The catalogs apps are listed from. Edit under Sources."
#define T_AREA_INSTALLED_NOTE "Records of installed apps and their origin."
#define T_AREA_INBOX_NOTE    ".pspdx files placed here can be installed via Direct Install."
#define T_AREA_CACHE_NOTE    "Downloaded catalogs and previews. Can be deleted."
#define T_AREA_PENDING_NOTE  "Temporary files of an installation in progress."
#define T_AREA_LOGS_NOTE     "Logs for troubleshooting."
#define T_AREA_SEED_NOTE     "Seed for secure connections. Not displayed."
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
#define T_STORAGE_ASK        "Install to"
#define T_STORAGE_INTERNAL   "System Storage"
#define T_STORAGE_CARD       "Memory Stick"
#define T_STORAGE_ABSENT     "Not available"
#define T_STORAGE_MISSING    "The installation storage is not available."

#define T_APP_FOLDER         "Folder: %s/PSP/GAME/%s"             /* device, dir */
#define T_APP_SOURCE         "Source: %s"                         /* url */
#define T_APP_CHECKED        "Checked: %s"                        /* when */
#define T_APP_VIA            "Via: %s"                            /* host */
#define T_APP_FILES_NOTE     "The two files PSPDX keeps for this app."
#define T_FILE_NOTE          "%s in %s"                           /* size, area */

#endif
