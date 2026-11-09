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
#define T_UMD_NONE           "No UMDs yet"
#define T_UMD_NONE_HINT      "Add a source with disc images under Sources."
#define T_UMD_VERSIONS       "  -  %d versions"   /* after the game's name in the grid */
#define T_PICK_VERSION       "Which version?"
/* The library: what is on the Memory Stick to be started, by section. */
#define T_HEAD_LIBRARY       "Launcher"
#define T_LIBRARY_PSP        "PSP Games"
#define T_LIBRARY_PS1        "PS1 Games"
#define T_LIBRARY_APPS       "Homebrew"
#define T_LIBRARY_FAVOURITES "Favorites"
#define T_STICK_FREE         "%s free on the Memory Stick"
#define T_ETA_SECONDS        "  -  %u s left"
#define T_ETA_MINUTES        "  -  %u min left"
#define T_ETA_HOURS          "  -  %u h %u min left"
#define T_FAVOURITE_ON       "Favorite: first in the list."
#define T_FAVOURITE_OFF      "No longer a favorite."
#define T_HINT_FAVOURITE     "Favorite"
#define T_HINT_LIST          "List"
#define T_HINT_GRID          "Icons"
#define T_LIBRARY_START      "Start"
#define T_LIBRARY_FAVOURITE  "Favorite"
#define T_LIBRARY_DELETE     "Delete"
#define T_LIBRARY_DELETE_ISO "The disc image is deleted from the Memory Stick."
#define T_LIBRARY_DELETE_FOLDER "Its folder is deleted from the Memory Stick, saved data aside."
#define T_LIBRARY_DELETED    "Deleted."
#define T_LIBRARY_DELETE_FAILED "Could not delete it."
#define T_LIBRARY_READING    "Reading the Memory Stick..."
#define T_LIBRARY_NONE       "Nothing to start yet"
#define T_LIBRARY_NONE_HINT  "Games in ISO and PSP/GAME on the Memory Stick are listed here."
#define T_HINT_START         "Start"
#define T_IN_BASKET          "In basket"

/* ------------------------------------------------------ waiting and status */

#define T_WORD_CONNECTING    "Connecting"
#define T_WORD_CHECKING      "Checking"
#define T_WORD_INBOX         "Checking INBOX"
#define T_WORD_OFFLINE       "Offline"
#define T_STATUS_CONNECTING  "Connecting..."
#define T_STATUS_JOINING     "Connecting to %s..."
#define T_STATUS_LOADING     "Loading catalog..."
#define T_STATUS_CHECKING    "Checking for updates..."
#define T_STATUS_ORIGIN      "Checking %.28s (%d of %d)..."       /* repository, at, of */
#define T_STALE              "%s has not been updated for %u day%s." /* host, days, "s" */
#define T_CATALOG_EMPTY      "No apps are available."
#define T_STATUS_UNREACHABLE "Could not load the catalog."
#define T_STATUS_TOO_LARGE   "The catalog is too large."
#define T_STATUS_OFFLINE     "Offline: last known releases"
#define T_NETCONF_UNAVAILABLE "The PSP could not open its network settings. Please share PSP/PSPDX/LOGS/pspdx.log."
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
#define T_PANEL_PLUGIN_OFF   "Version %s, off"                    /* version: an installed plugin */
#define T_PANEL_NO_RELEASE   "No release"
#define T_PANEL_UNSUPPORTED  "Not installable yet"
#define T_PANEL_IN_BASKET    "   In basket"

/* On the card, in place of a picture. */
#define T_CARD_LOADING       "loading"
#define T_CARD_NO_PICTURE    "no picture"

/* ---------------------------------------------------------------- options */

#define T_MENU_RUN           "Run"
#define T_MENU_RESTART       "Restart"
#define T_MENU_PLUGIN_ON     "Turn On"                            /* a plugin's row in place of Run */
#define T_MENU_PLUGIN_OFF    "Turn Off"
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
#define T_HINT_EDIT          "Edit"

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
/* A plugin is installed and turned on in two steps, and asked about the
   second: the custom firmware loads it only once it is turned on, and only
   when the PSP starts. Every line here fits its band; dev/tests holds them
   to it. */
#define T_PLUGIN_INSTALLED_ASK "%s was installed."                /* name: the title, the next the question */
#define T_PLUGIN_TURN_ON_LINE "Do you want to turn it on?"
#define T_PLUGIN_ON_ASK      "Do you want to turn it on?"
#define T_PLUGIN_OFF_ASK     "Do you want to turn it off?"
#define T_PLUGIN_ON_LINE     "It is loaded after a restart. Hold START at power-on to skip plugins."
#define T_PLUGIN_OFF_LINE    "It is unloaded after a restart."
#define T_ALL_ASK_INSTALL    "Do you want to install %d app%s?"   /* n, "s" */
#define T_ALL_ASK_UPDATE     "Do you want to update %d app%s?"    /* n, "s" */
#define T_ALL_SIZE           "%s in total"                        /* size */
#define T_ALL_AGAIN          ", %d again"                         /* n */
#define T_ALL_SKIPPED        ", %d skipped"                       /* n */
#define T_RUN_ASK            "Do you want to start %s?"           /* name */
#define T_RUN_LINE           "PSPDX will close."
#define T_RESTART_ASK        "Do you want to restart PSPDX now?"
#define T_RESTART_LINE       "Version %s takes effect after restarting." /* version, at most 20 bytes */
#define T_RESTART_LATER      "Restart PSPDX to finish the update."
#define T_INBOX_ASK          "Do you want to install %d app%s from INBOX?" /* n, plural */
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
#define T_DOWNLOAD_OFFLINE   "Could not connect. Nothing was downloaded."
#define T_BAKED_PAUSED       "Baked UI is paused while downloads run."
#define T_BAKED_MISSING      "Baked UI could not be loaded."
#define T_OK                 "OK"                                /* an error band's one answer */

/* ---------------------------------------------------------------- results */

#define T_INSTALLED          "%s %s was installed."               /* name, version */
#define T_INSTALLED_NO_PSPDX "%s %s was installed. Its repository has no .pspdx." /* name, version */
#define T_INSTALLED_FROM_FILE "%s %s was installed from your file; the repository has no .pspdx." /* name, version */
#define T_UPDATED_SELF       "PSPDX was updated to %s."           /* version */
/* A plugin, in the status line: when it takes effect, and once it is on
   the way to start without it. */
#define T_PLUGIN_ON          "On after restart. Hold START to skip plugins."
#define T_PLUGIN_OFF         "Off after restart."
#define T_PLUGIN_UPDATED     "%s %s updated. Please restart." /* name, version: an update */
#define T_PLUGIN_REMOVED     "%s was deleted. Off after restart." /* name */
#define T_PLUGIN_REMOVED_LINES "Deleted. PLUGINS.TXT still names it."
#define T_PLUGIN_LEFT        "No longer managed. Its files and line stay."
#define T_PLUGIN_KEPT        "Deleted. Your own files in its folder stay."
#define T_PLUGIN_NOT_OURS    "Not changed: other lines name it."
#define T_PLUGIN_FAILED      "Could not change PLUGINS.TXT."
#define T_PLUGIN_FAILED_WHY  "Not changed: %s."                   /* reason */
#define T_PLUGIN_NOT_INSTALLED "Not installed: %s."               /* reason */
#define T_PLUGIN_NOT_DELETED "Not deleted: %s."                   /* reason */
#define T_CANCELLED          "Installation of %s was canceled."   /* name */
#define T_INSTALL_FAILED     "Could not install %s (%d)."         /* name, code */
#define T_INSTALL_FAILED_WHY "Could not install %s: %s."          /* name, reason */
#define T_LAYOUT_HEAD        "Check before installing"
#define T_LAYOUT_WHY         "%d EBOOT.PBP in the zip; the top one is the app."
#define T_LAYOUT_INSTALLS    "Installs"
#define T_LAYOUT_FROM        "from %s in the zip"                /* folder */
#define T_LAYOUT_TOP_SHORT   "the top"
#define T_LAYOUT_ALONG       "Comes along"
#define T_LAYOUT_UNUSED      "copied along, never started"
#define T_LAYOUT_NOTHING     "Nothing"
#define T_LAYOUT_MORE        "and %d more EBOOT.PBP"
#define T_LAYOUT_LEFT_OUT    "Left out"
#define T_LAYOUT_WHY_NAMES   "Some file names in the zip are not UTF-8."
#define T_LAYOUT_RENAMED     "Renamed"
#define T_LAYOUT_MORE_NAMES  "and %d more names"
#define T_LAYOUT_RENAMED_SJIS "read as Japanese (Shift-JIS)"
#define T_LAYOUT_RENAMED_437 "read as DOS Latin (CP437)"
#define T_LAYOUT_MORE_FILES  "and %d more folders"
#define T_LAYOUT_MAC         "__MACOSX/  (Mac metadata)"
#define T_LAYOUT_ONLY_MAC    "Only Mac metadata (__MACOSX/)"
#define T_LAYOUT_YES         "Install"
#define T_LAYOUT_NO          "Don't install"
#define T_DECLINED           "%s was not installed."             /* name */
#define T_WHY_STICK          "the Memory Stick could not be written"
#define T_WHY_NO_ROOM        "the Memory Stick is full"
#define T_WHY_NO_ANSWER      "the server did not answer"
#define T_WHY_STATUS         "the server answered %ld"           /* HTTP status */
#define T_WHY_SIZE           "the download was the wrong size"
#define T_WHY_HASH           "the download did not match its checksum"
#define T_WHY_ISO_TAKEN      "ISO/ has a file of that name already"
#define T_WHY_EBOOTS         "its zip holds several EBOOT.PBP side by side"
#define T_WHY_NO_EBOOT       "its zip holds no EBOOT.PBP"
#define T_WHY_ZIP            "its zip cannot be unpacked safely"
#define T_WHY_NAME           "its zip names a file in a foreign code page"
#define T_WHY_NO_PRX         "its zip holds no .prx"
#define T_WHY_PRXS           "its .pspdx must name a .prx"
#define T_WHY_PRX_NAMED      "the .prx it names is missing"
#define T_WHY_PRX_FOLDERS    "its zip has .prx in two folders"
#define T_WHY_PRX_FILES      "its zip holds too many files"
#define T_WHY_PRX_NAME       "its .prx has an unusable name"
#define T_WHY_PRX_RENAMED    "its .prx has another name now"
#define T_WHY_PRX_THERE      "seplugins/%s is in the way" /* the plugin's name: a folder or a file */
#define T_WHY_PRX_COPY       "an old copy of it is in the way"
#define T_WHY_PRX_READONLY   "one of its files is read-only"
#define T_WHY_PRX_CHANGED    "its .prx was changed by hand"
#define T_WHY_LIST_READ      "PLUGINS.TXT cannot be read"
#define T_WHY_LIST_LARGE     "PLUGINS.TXT is too large"
#define T_WHY_LIST_TEXT      "PLUGINS.TXT is not plain text"
#define T_WHY_LIST_READONLY  "PLUGINS.TXT is read-only"
#define T_WHY_LIST_CHANGED   "PLUGINS.TXT has changed"
#define T_WHY_LIST_WRITE     "PLUGINS.TXT was not written"
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
#define T_START_REFUSED      "Could not start the app (%08x)."      /* the code the firmware gave */
#define T_SELF_DELETE        "PSPDX cannot delete itself."

/* Direct Install and sources. */
#define T_OSK_GITHUB         "Enter GitHub owner/repository"
#define T_OSK_SOURCE         "Enter source URL"
#define T_OSK_PASSWORD       "Password"
#define T_SOURCE_EDIT        "Edit"
#define T_SOURCE_PASSWORD    "Password"
#define T_SOURCE_DELETE      "Delete"
#define T_SOURCE_TUNNEL      "Tailscale"
#define T_SOURCE_TEST        "Test"
#define T_TEST_RUNNING       "Reaching the source..."
#define T_TEST_DIRECT        "directly"
#define T_TEST_TUNNEL        "through Tailscale"
#define T_TEST_OK            "Reached %s in %.1f s."
#define T_TEST_401           "Reached, but the password is wrong or missing."
#define T_TEST_STATUS        "Reached, but it answered %ld."
#define T_TEST_NO_TUNNEL     "Not reached through Tailscale: %s"
#define T_TEST_NOTHING       "No answer from the source."
#define T_TUNNEL_REMOVE      "Remove"
#define T_TUNNEL_REMOVE_ASK  "Remove Tailscale?"
#define T_TUNNEL_REMOVE_LINE "This PSP leaves the tailnet, and its sources go direct again."
#define T_TUNNEL_REMOVED     "Removed. This PSP has left the tailnet."
#define T_TUNNEL_REMOVED_OFF "Removed here. Delete the device in the admin console too."
#define T_TUNNEL_NOT_REMOVED "Could not delete tunnel.key."
#define T_SOURCE_TUNNEL_ON   "Reached through Tailscale."
#define T_SOURCE_TUNNEL_OFF  "Reached directly."
#define T_SOURCE_TUNNEL_FULL "Too many sources through Tailscale: eight at most."
#define T_VALUE_ON           "On"
#define T_VALUE_OFF          "Off"
#define T_VALUE_ERROR        "Error"
/* Tailscale under the gear, its questions and what it came to. */
#define T_TUNNEL_NO_FILE     "Put your auth key in PSP/PSPDX/tunnel.txt."
#define T_TUNNEL_NO_KEY      "tunnel.txt holds no tskey-auth- key."
#define T_TUNNEL_TIMEOUT     "No answer within a minute. Try again."
#define T_TUNNEL_CANCELLED   "Cancelled."
#define T_TUNNEL_KEPT        "Could not delete tunnel.txt."
#define T_NOTE_TUNNEL_ON     "Set up. Mark a source Through Tailscale in its options."
#define T_TUNNEL_AGAIN       "A new key in tunnel.txt sets it up again."
#define T_TUNNEL_NOT_SAVED   "Connected, but tunnel.key was not saved."
#define T_TUNNEL_GONE        "tunnel.txt deleted."
#define T_SOURCE_PASSWORD_SET "Password saved."
#define T_SOURCE_PASSWORD_GONE "Password removed."
#define T_BAD_ADDRESS        "The address is not valid."
#define T_GITHUB_FORMAT      "Enter it as owner/repository."
#define T_SOURCE_UNAVAILABLE "Could not add the source."
#define T_SOURCE_SAVED_OFFLINE "Saved. It did not answer now and is tried again at every refresh."
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
#define T_SET_XMB            "XMB"
#define T_SET_UI             "UI"
#define T_SET_NETWORK        "Network"
#define T_SET_CONNECTION     "Offline"
#define T_SET_ONLINE         "Online"
#define T_SET_CONNECTING     "Connecting..."
#define T_NOTE_ONLINE        "X disconnects. What was fetched last stays."
#define T_NOTE_OFFLINE       "X connects through the system's dialog, where you choose the network you are in."
#define T_DISCONNECTED       "Disconnected."
#define T_NOTE_NETWORK       "Where the catalogs come from, and how PSPDX gets online: Wi-Fi, the USB cable or Tailscale."
#define T_SET_SOURCES        "Sources"
#define T_SET_SYSTEM         "Options"
#define T_SET_CABLE          "USBNet"
#define T_SET_TUNNEL         "Tailscale"
#define T_SET_FILES          "Data"
#define T_SET_ABOUT          "About"
#define T_SET_DIRECT         "Direct Install"
#define T_NOTE_XMB           "Leaves PSPDX for the PSP's home menu."
#define T_XMB_FAILED         "Could not turn the launcher off for the home menu."
#define T_NOTE_SOURCES       "Manages the catalogs apps are listed from."
#define T_NOTE_SYSTEM        "Adjusts display, testing and default settings."
#define T_NOTE_CABLE         "Connects over Wi-Fi or the USB cable. Turn USBNet off or delete it under Installed."
#define T_NOTE_TUNNEL        "Joins your tailnet with the auth key in PSP/PSPDX/tunnel.txt."
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
#define T_VALUE_FPS30        "30 FPS Baked"
#define T_VALUE_FPS60        "60 FPS Mercy"
#define T_SYS_FPS            "Show FPS"
#define T_SYS_DEV            "Fake Updates"                      /* every installed package is said to have an update */
#define T_SYS_UNRELEASED     "Show Unreleased"                   /* catalog entries tagged "unreleased" */
#define T_SYS_SWEEP          "Renew TLS Seed"
#define T_SYS_RESET_ALL      "Restore Defaults"
#define T_SYS_FRAME_RATE_NOTE "Switches between baked UI at 30 FPS and performance UI at 60 FPS."
#define T_SYS_FPS_NOTE       "Displays the frame rate on screen."
#define T_SYS_DEV_NOTE       "Testing option to simulate updates for installed apps."
#define T_SYS_UNRELEASED_NOTE "Also lists apps their authors tagged unreleased. Until PSPDX restarts."
#define T_SYS_DEVELOPER      "Developer Mode"
#define T_SYS_DEVELOPER_NOTE "For testing: lists apps tagged unreleased and pretends every installed app has an update. Until PSPDX restarts."
#define T_RESET_SETTINGS     "Restore Settings"
#define T_SUB_AUTO_OFF       "Off"
#define T_SYS_FILL           "Autocache"
#define T_SYS_FILL_NOTE      "While PSPDX is left alone, fetches every app's icon so lists show at once. Any key pauses it."
#define T_FILLING            "Filling cache for faster browsing  %d%%"
#define T_SYS_LAUNCHER       "Launcher"
#define T_SYS_AUTO           "Auto Connect"
#define T_SYS_AUTO_NOTE      "Connects in the background at every start, without the system's dialog. The foot shows how far it got."
#define T_SYS_NETWORK        "Connection"
#define T_SYS_NETWORK_NOTE   "The connection Auto Connect uses, from the system's network settings. X goes to the next one."
#define T_SYS_NETWORK_NONE   "None"
#define T_SYS_NO_NETWORKS    "No connections yet. Set one up under Network Settings in the home menu."
#define T_CONNECT            "Connect"
#define T_CONNECT_NOTE       "Shows what was fetched last time. Connect to see what is new: the system's dialog asks which connection."
#define T_CONNECT_WAIT       "Still reading what was kept..."
#define T_STATE_NOW          "Now: %s"
#define T_STATE_AUTO_ON      "Now: on, with %s"
#define T_STATE_AUTO_OFF     "Now: off"
#define T_OFFLINE_TITLE      "Not connected"
#define T_OFFLINE_HINT       "Connect to see what is published."
#define T_HINT_CONNECT       "Connect"
#define T_HINT_NEXT          "Next"
#define T_SYS_LAUNCHER_NOTE  "Starts PSPDX instead of the home menu (ARK). SELECT at power-on still opens the home menu."
#define T_LAUNCHER_ON        "PSPDX now starts instead of the home menu."
#define T_LAUNCHER_OFF       "The home menu starts again."
#define T_LAUNCHER_NO_ARK    "ARK was not found in PSP/SAVEDATA/ARK_01234."
#define T_LAUNCHER_NO_SETTINGS "ARK's SETTINGS.TXT was not found."
#define T_LAUNCHER_NO_SELF   "PSPDX's EBOOT.PBP was not found in PSP/GAME."
#define T_LAUNCHER_FAILED    "Could not change the launcher on the Memory Stick."
#define T_SYS_SWEEP_NOTE    "Generates a new seed for secure connections."
#define T_SYS_RESET_NOTE     "Restores all settings to their defaults. Installed apps are not deleted."
#define T_HINT_CHANGE        "Change"
#define T_HINT_TOGGLE        "Toggle"
#define T_HINT_RUN           "Run"
#define T_HINT_RESTORE       "Restore"
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
#define T_CATEGORY_EMPTY     "No apps in this category."
#define T_TAG_NOTE           "%d app%s with this tag: "      /* n, "s"; the names follow */
#define T_SOURCE_NOTE        "%d app%s from this source: "   /* n, "s"; the names follow */
/* The three rows the Homebrew tab opens with, the ways it is browsed by. */
#define T_SEARCH             "Search"
#define T_SEARCH_HEAD        "Search: %s"
#define T_SEARCH_NOTE        "Find apps by name, author, tag or description."
#define T_SEARCH_LAST        "Last search: %s"
#define T_SEARCH_NONE        "No apps found for \"%s\""
#define T_SEARCH_NONE_UMD    "No UMDs found for \"%s\""
#define T_SEARCH_NONE_GAMES  "No games found for \"%s\""
#define T_OSK_SEARCH_UMD     "Search UMDs"
#define T_OSK_SEARCH_GAMES   "Search games"
#define T_HEAD_GENRES        "Genres"
#define T_BROWSE_GENRE       "Browse by Genre"
#define T_HINT_ALL           "All"
#define T_OSK_SEARCH         "Search apps"
#define T_SORT_NEWEST        "Sorted by newest"
#define T_SORT_NAME          "Sorted by name"
#define T_SORT_INSTALLED     "Installed first"
#define T_BROWSE_CATEGORY    "Browse by Category"
#define T_BROWSE_TAG         "Browse by Tag"
#define T_BROWSE_SOURCE      "Browse by Source"
/* The header over each way's rows. */
#define T_HEAD_CATEGORIES    "Categories"
#define T_HEAD_TAGS          "Tags"
#define T_BROWSE_NONE        "Nothing to browse by yet."
/* Every repository typed into Direct Install, as one source. */
#define T_SOURCE_DIRECT      "Direct installs"
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
/* The first start with the cable's plugin beside the EBOOT, and the way
   through the system's dialog the first time the cable is used. */
#define T_CABLE_ASK          "Connect via"
#define T_CABLE_WIFI         "Wi-Fi"
#define T_CABLE_USB          "USB cable"
/* The cable only works while the gateway runs on the PC: looked for
   first, and where it is to be had said when it is not there. The address
   in one piece, for whoever points at it. A plugin too old to look is
   asked about instead. */
#define T_CABLE_INSTALL_ASK  "Activate USB?"
#define T_CABLE_INSTALL_LINE "This installs the USBNet plugin and turns it on."
#define T_CABLE_GATEWAY_URL  "github.com/chriopter/pspkit-usbnet"
#define T_CABLE_GATEWAY_ASK  "Is the gateway running on your PC?"
#define T_CABLE_GATEWAY_LINE "Download it at " T_CABLE_GATEWAY_URL
#define T_CABLE_SEARCH_ASK   "Search for the gateway on your PC?"
#define T_CABLE_LOOKING      "Looking for the gateway on your PC..."
#define T_CABLE_FOUND_ASK    "Gateway found. Do you want to connect now?"
#define T_CABLE_NONE         "Gateway not found"
#define T_CABLE_NONE_LINE    "Start it on your PC. " T_CABLE_GATEWAY_LINE
#define T_CABLE_RETRY        "Retry"
#define T_CABLE_GO_ON_LINE   "Do you want to go on with the cable?"
#define T_CABLE_CONNECT_ASK  "Do you want to connect now?"
#define T_CABLE_CONNECT_LINE "Choose [New Connection], then Scan, then \"Hi-Speed USB\"."
#define T_CABLE_OFF          "Wi-Fi is used. The plugin stays installed."
#define T_CABLE_RESTART      "Restart your PSP to use the updated USB plugin."
#define T_CABLE_NOT_LOADED   "The USB plugin could not be loaded."
#define T_STORAGE_ASK        "Install to"
#define T_STORAGE_INTERNAL   "System Storage"
#define T_STORAGE_CARD       "Memory Stick"
#define T_STORAGE_ABSENT     "Not available"
#define T_STORAGE_MISSING    "The installation storage is not available."

#define T_APP_FOLDER         "Folder: %s/PSP/GAME/%s"             /* device, dir */
#define T_APP_PLUGIN         "File: %s"                           /* ms0:/seplugins/<name>/<name>.prx, ms0:/ISO/<file> */
#define T_APP_SOURCE         "Source: %s"                         /* url */
#define T_APP_CHECKED        "Checked: %s"                        /* when */
#define T_APP_VIA            "Via: %s"                            /* host */
#define T_APP_FILES_NOTE     "The two files PSPDX keeps for this app."
#define T_FILE_NOTE          "%s in %s"                           /* size, area */

#endif
