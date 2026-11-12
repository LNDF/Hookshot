/* The game's data folder (hos_data.h): chosen once at the start, and the files a build from before kept loose in the game folder
   put where they go now.

   Which folder: --user <folder> when given; else, for a game started with nothing (as a player starts it: image.bin in the
   game folder), %LOCALAPPDATA%\Hookshot on Windows and $XDG_DATA_HOME/hookshot (~/.local/share/hookshot) on Linux; else (a
   development run given its image.bin) "user" in the current folder, as before. A builder's folder whose platform folder cannot
   be made keeps <game>/user. A portable install (#2367's Builder: .hookshot_builder_home in the game folder) keeps its data
   folder in the game folder too, <game>/user, so it goes with the drive the game is on.

   What goes where:
   - the data folder: the saves and the settings (hookshot_options.ini, hookshot_env.txt), what belongs to the player;
   - <game>/cache: the shader and pipeline caches, made for this build and this GPU, several GB, beside the game the player put
     on the drive with room for it, and the button prompts made from the player's controls (cache/prompts: host/prompts.c).
     Its name says it can be deleted: the next start makes it again. One cache per game folder, so a release and a nightly
     side by side do not throw each other's away;
   - <game>/logs: hookshot_log.txt and the runs before it, and the other logs and traces a run writes.

   The move, each entry once and never over one already there:
   - for the platform folder only (a --user folder or a development run is the caller's to fill): the entries of <game>/user (the
     saves; on Linux since an earlier build already, on Windows now) into the data folder, a rename, else a copy and delete
     (another drive). All or nothing: when one of its entries is in the data folder already, or one cannot move (whatever moved is
     moved back), the saves stay in <game>/user, as they were, and nothing is lost or split (saves in both places are said in
     saves-in-two-places.txt, neither picked nor merged); the data folder is then <game>/user too, so that the settings stay with
     the saves (a second game folder beside the first keeps its own of both). A copy whose original cannot be deleted after (a file
     held or read-only) leaves <game>/user renamed user.moved, so that a later start does not take the leftover for the saves. A
     link (a symbolic link, a Windows junction) is never renamed or copied through: on Linux it is made again in the data folder,
     to the same folder (#2282); on Windows the saves stay with it. A user/ that is a link itself is the player's choice of place,
     used as it is. The settings, each on its own; one that cannot move stays, and the log says it is not read. For a portable
     install, the settings alone, into <game>/user;
   - whenever the game folder is known: its loose caches into cache/ and its logs into logs/ (renames on the same drive), the
     Vulkan pipeline cache renamed from mkw_vk_pipelines*.bin to hookshot_vk_pipelines*.bin on the way; the button prompts an
     older build made in the game folder's prompts/ (Common.*, External.bfotf.zs) are deleted: they are made again in cache/.
   language.txt is not moved: it is the build's, which the builder writes beside the game (nx2/make_game.py). */
#include "hos_paths.h"   /* (first: windows.h, host/posix's on Linux) */
#include "hos_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define SEP 92
#define SEP_S "\\"
#else
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define SEP '/'
#define SEP_S "/"
#endif

extern const char *hos_user_dir;   /* (nn_fs.c: the saves) */
void hos_log(const char *fmt, ...);   /* (hos.h; declared here so tests/test_data_dir.py builds this file alone) */

static char data_dir[MAX_PATH] = "user";
static int data_ready;

/* (what the move did, for the log once it is open) */
static int moved_files, tidied_files, kept_files, saves_moved, saves_kept;
static char set_aside[MAX_PATH];   /* (<game>/user renamed after a copy whose original could not be deleted whole) */
/* (saves_link: the saves hold a link, kept where they are, or are one (2); portable: a portable install's data folder;
   saves_twice: saves both in <game>/user and in the data folder) */
static int saves_link, portable, saves_twice;
static int leftover;   /* (1: a leftover of an earlier move set aside; 2: found, but still held where it is) */
static char unread_options[MAX_PATH], unread_env[MAX_PATH];   /* (a settings file beside the game the game does not read) */
static char kept_first[MAX_PATH];
static char saves_from[MAX_PATH];

/* The settings a game folder had, moved into the data folder; its logs into logs/, its caches into cache/ */
static const char *const to_data[] = {"hookshot_options.ini", "hookshot_env.txt"};
static const char *const to_logs[] = {
    "hookshot_log.txt", "hookshot_log.1.txt", "hookshot_log.2.txt", "hookshot_log.3.txt", "hookshot_log.4.txt",
    "hookshot_startup.log", "hookshot_frames.csv",
};
static const char *const to_cache[] = {
    "hookshot_programs.bin", "hookshot_programs.idx", "hookshot_pipelines.bin", "hookshot_computes.bin",
    "hookshot_pipeline_states.bin", "hookshot_pipeline_states_v1.bin", "hookshot_prepared.bin", "hookshot_prepare_skipped",
    "hookshot_pipelines.bin.running",   /* (the precompile's crash count, beside its notes: vk_draw.cpp precompile_run) */
    "hookshot_driver_faults.txt",       /* (the programs this PC's driver faulted on: vk_draw.cpp driver_faults_path) */
};
/* The Vulkan pipeline cache, mkw_vk_pipelines.bin and its play files mkw_vk_pipelines_play_<n>.bin (mkw-recomp's names), renamed
   hookshot_vk_pipelines*.bin as they move into cache/ (gpu/vk/vk_draw.cpp) */
static const char old_pcache[] = "mkw_vk_pipelines", new_pcache[] = "hookshot_vk_pipelines";
/* Made from the player's choices and made again when missing: deleted from the game folder */
static const char *const made_again[] = {
    "prompts" SEP_S "Common.bntx.zs", "prompts" SEP_S "Common.blarc.zs", "prompts" SEP_S "Common.key",
    "prompts" SEP_S "Common.blarc.key", "prompts" SEP_S "External.bfotf.zs", "prompts" SEP_S "rsizetable.mod.zs",
};

static int exists(const char *p) {
#ifndef _WIN32
    struct stat st;   /* (a link that names nothing is there too) */
    if (lstat(p, &st) == 0) return 1;
#endif
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
static int is_dir(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
/* a symbolic link or a Windows junction (host/posix marks a symbolic link so too) */
static int is_link(const char *p) {
#ifndef _WIN32
    struct stat st;   /* (lstat: one that names nothing is a link too) */
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
#else
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#endif
}
#ifndef _WIN32
/* (linux, #2282) A symbolic link in the saves, to a folder the player keeps elsewhere (a synced folder, another drive), is made
   again in the new place to the same folder, never entered or copied through: written in full (realpath), so that one written
   from where it stood ("../../sync") still names it; one to an entry that moves too names that entry in the new place
   (move_old, move_new); one that names nothing goes as it was written */
static const char *move_old, *move_new;   /* (the folder on the move, as realpath names it, and where it goes) */
static int copy_link(const char *from, const char *to) {
    char target[PATH_MAX], there[PATH_MAX];
    const size_t old = move_old ? strlen(move_old) : 0;
    if (!realpath(from, target)) {
        const ssize_t n = readlink(from, target, sizeof target - 1);
        if (n < 0) return 0;
        target[n] = 0;
    } else if (old && !strncmp(target, move_old, old) && target[old] == SEP) {
        snprintf(there, sizeof there, "%s%s", move_new, target + old);
        snprintf(target, sizeof target, "%s", there);
    }
    return symlink(target, to) == 0;
}
#endif
/* (a link is never copied through: tree_has_link says whether a copy would) */
static int tree_has_link(const char *p) {
    if (is_link(p)) return 1;
    if (!is_dir(p)) return 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int found = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", p, fd.cFileName);
            found = tree_has_link(a);
        } while (!found && FindNextFileA(h, &fd));
        FindClose(h);
    }
    return found;
}
static int copy_tree(const char *from, const char *to) {
#ifndef _WIN32
    if (is_link(from)) return copy_link(from, to);
#endif
    if (!is_dir(from)) {
        FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
        int ok = in && out;
        static char buf[1 << 16];
        for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
        ok = ok && in && !ferror(in);
        if (in) fclose(in);
        if (out && fclose(out)) ok = 0;
        return ok;
    }
    if (!CreateDirectoryA(to, NULL) && !is_dir(to)) return 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", from);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int ok = 1;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH], b[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", from, fd.cFileName);
            snprintf(b, sizeof b, "%s" SEP_S "%s", to, fd.cFileName);
            ok = copy_tree(a, b);
        } while (ok && FindNextFileA(h, &fd));
        FindClose(h);
    }
    return ok;
}
/* p and what is in it; a link only itself, never what it points to */
static void delete_tree(const char *p) {
    if (is_link(p)) {
        if (!RemoveDirectoryA(p)) DeleteFileA(p);   /* (a junction or a link to a folder on Windows; a symbolic link on Linux) */
        return;
    }
    if (!is_dir(p)) {
        DeleteFileA(p);
        return;
    }
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", p, fd.cFileName);
            delete_tree(a);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(p);
}
/* from to to, which is not there yet: a rename, else (another drive) a copy and delete, never through a link. 1 moved, 0 left
   where it was. *left: set when the copy is whole but some of the original could not be deleted (held, read-only) */
static int move_entry(const char *from, const char *to, int *left) {
#ifdef _WIN32
    /* (a link or junction, or a folder holding one, is never moved: renamed into another folder, a relative one names nothing
       there, and copied, the folder it names would be emptied after) */
    if (tree_has_link(from)) return 0;
    if (MoveFileExA(from, to, 0)) return 1;
#else
    /* (a folder holding a link is copied, its links made again (copy_link), never renamed: from another folder a link written
       from where it stood names nothing) */
    if (!tree_has_link(from) && MoveFileExA(from, to, 0)) return 1;
#endif
    if (copy_tree(from, to)) {
        delete_tree(from);
        if (exists(from) && left) *left = 1;
        return 1;
    }
    delete_tree(to);   /* (a copy cut short) */
    return 0;
}

/* (the clock, in the file system's ticks: a leftover of a move is told from saves written since by its files' times) */
static long long now_ticks(void) {
#ifdef _WIN32
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return (long long)(((unsigned long long)t.dwHighDateTime << 32) | t.dwLowDateTime);
#else
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
#endif
}
static long long file_ticks(const char *p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &d)) return 0;
    return (long long)(((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime);
#else
    struct stat st;
    if (lstat(p, &st)) return 0;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}
/* the newest write of a file under p (the files, not the folders: deleting entries changes a folder's time) */
static long long newest_file(const char *p) {
    if (is_link(p) || !is_dir(p)) return file_ticks(p);
    long long newest = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", p, fd.cFileName);
            const long long t = newest_file(a);
            if (t > newest) newest = t;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return newest;
}

/* The record of the moves: .hookshot-moved in the data folder, a line "<ticks> <folder>" for each <game>/user moved into it.
   A later start that finds such a folder again, holding only files from before its move, takes it for what the move could not
   delete (a file held open, read-only), sets it aside and keeps the data folder; it never takes it for the saves. Files written
   since (an older build started in that game folder after the move) are saves of their own: saves in two places */
static long long moved_at(const char *beside) {
    char path[MAX_PATH], line[MAX_PATH + 32];
    long long at = 0;
    if (!hos_data_path(path, sizeof path, ".hookshot-moved")) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        sp[strcspn(sp, "\r\n")] = 0;
        if (!strcmp(sp + 1, beside)) at = strtoll(line, NULL, 10);
    }
    fclose(f);
    return at;
}
static void record_move(const char *beside) {
    char path[MAX_PATH];
    if (!hos_data_path(path, sizeof path, ".hookshot-moved")) return;
    FILE *f = fopen(path, "a");
    if (f) fprintf(f, "%lld %s\n", now_ticks(), beside), fclose(f);
}
/* <game>/user renamed user.moved (or .moved.<n>), so that no later start takes what is in it for saves: 1 when it was */
static int put_aside(const char *beside) {
    char aside[MAX_PATH];
    for (int n = 1; n < 100; n++) {
        snprintf(aside, sizeof aside, n == 1 ? "%s.moved" : "%s.moved.%d", beside, n);
        if (!exists(aside) && MoveFileExA(beside, aside, 0)) {
            snprintf(set_aside, sizeof set_aside, "%s", aside);
            return 1;
        }
    }
    return 0;
}

/* <game>/user's entries into the data folder, all or none: the folder the saves are in after it. Each entry is renamed (the
   same drive) or copied (another drive, or holding a link on Linux); no original is deleted before every entry has gone. One
   that cannot go undoes the others (the renamed renamed back, the copies deleted: they are ours), so the saves stay whole in
   <game>/user, which nothing has touched. Only then are the copied originals deleted: what cannot be (a file held open,
   read-only) is a leftover, set aside as user.moved, and the move is recorded (moved_at) so that a later start never takes
   a leftover for the saves, even when it could not be set aside */
enum { MAX_SAVE_ENTRIES = 64 };
static const char *move_saves(const char *beside) {
    if (!is_dir(beside)) return data_dir;
    snprintf(saves_from, sizeof saves_from, "%s", beside);
    if (is_link(beside)) {   /* (a user/ that is a link itself is the player's choice of place: used as it is, nothing moves) */
        saves_link = 2;
        return beside;
    }
    const long long moved = moved_at(beside);
    if (moved && newest_file(beside) <= moved) {   /* (what an earlier move could not delete: not saves, whatever its names) */
        leftover = 1;
        if (!put_aside(beside)) leftover = 2;   /* (still held: the next start tries again; the data folder is used meanwhile) */
        return data_dir;
    }
#ifndef _WIN32
    char real[PATH_MAX];
    move_old = realpath(beside, real), move_new = data_dir;   /* (copy_link: links to entries that move too follow them) */
#endif
    static char names[MAX_SAVE_ENTRIES][MAX_PATH];
    static char how[MAX_SAVE_ENTRIES];   /* (1 renamed, 2 copied) */
    int count = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", beside);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return data_dir;
    /* (first, whether every entry can go: none in the data folder already, and not more than are kept track of) */
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char to[MAX_PATH];
        snprintf(to, sizeof to, "%s" SEP_S "%s", data_dir, fd.cFileName);
        char from[MAX_PATH];
        snprintf(from, sizeof from, "%s" SEP_S "%s", beside, fd.cFileName);
#ifdef _WIN32
        if (tree_has_link(from)) saves_link = 1, saves_kept++;   /* (a link or junction: the saves stay with it, whole) */
        else
#endif
        if (exists(to) || count == MAX_SAVE_ENTRIES) saves_kept++;
        else snprintf(names[count++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (saves_kept) return beside;
    for (int i = 0; i < count; i++) {   /* (every entry over: renamed, or copied with its original kept for now) */
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof from, "%s" SEP_S "%s", beside, names[i]);
        snprintf(to, sizeof to, "%s" SEP_S "%s", data_dir, names[i]);
        how[i] = !tree_has_link(from) && MoveFileExA(from, to, 0) ? 1 : copy_tree(from, to) ? 2 : 0;
        if (how[i]) continue;
        delete_tree(to);   /* (a copy cut short) */
        /* (one could not go: the others come back as they were, and the saves stay whole in <game>/user) */
        saves_kept = count - i;
        for (int k = i - 1; k >= 0; k--) {
            snprintf(from, sizeof from, "%s" SEP_S "%s", beside, names[k]);
            snprintf(to, sizeof to, "%s" SEP_S "%s", data_dir, names[k]);
            if (how[k] == 2) delete_tree(to);   /* (a copy: its original is still there, untouched) */
            else if (!MoveFileExA(to, from, 0)) saves_kept++;   /* (a rename back on the same drive: kept where it went if not) */
        }
        return beside;
    }
    saves_moved = count;
    int left = 0;
    for (int i = 0; i < count; i++)   /* (every copy whole: the originals go now) */
        if (how[i] == 2) {
            char from[MAX_PATH];
            snprintf(from, sizeof from, "%s" SEP_S "%s", beside, names[i]);
            delete_tree(from);
            left |= exists(from);
        }
    record_move(beside);
    if (!RemoveDirectoryA(beside) && (left || exists(beside))) put_aside(beside);
    return data_dir;
}

/* game's file name into the folder dir (with its separator): 1 when it moved */
static int move_as(const char *game, const char *name, const char *dir, const char *as) {
    char from[MAX_PATH], to[MAX_PATH];
    if (snprintf(from, sizeof from, "%s%s", game, name) >= (int)sizeof from || !exists(from) || is_dir(from)) return 0;
    if (snprintf(to, sizeof to, "%s%s", dir, as) >= (int)sizeof to) return 0;
    if (!exists(to) && move_entry(from, to, NULL)) return 1;
    if (!kept_files++) snprintf(kept_first, sizeof kept_first, "%s", from);
    return 0;
}
static int move_file(const char *game, const char *name, const char *dir) { return move_as(game, name, dir, name); }

/* folder's mkw_vk_pipelines*.bin into cache as hookshot_vk_pipelines*.bin (folder and cache: with their separators) */
static int move_pcache(const char *folder, const char *cache) {
    int moved = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s%s*.bin", folder, old_pcache);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char as[MAX_PATH];
        if (strncmp(fd.cFileName, old_pcache, sizeof old_pcache - 1)) continue;
        snprintf(as, sizeof as, "%s%s", new_pcache, fd.cFileName + sizeof old_pcache - 1);
        moved += move_as(folder, fd.cFileName, cache, as);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return moved;
}

/* the game folder's loose caches and logs of a build from before into its cache/ and logs/ (game: the folder, with its
   separator) */
static void tidy_game(const char *game) {
    char cache[MAX_PATH], logs[MAX_PATH];
    snprintf(cache, sizeof cache, "%scache" SEP_S, game);
    snprintf(logs, sizeof logs, "%slogs" SEP_S, game);
    for (size_t i = 0; i < sizeof to_logs / sizeof *to_logs; i++) tidied_files += move_file(game, to_logs[i], logs);
    for (size_t i = 0; i < sizeof to_cache / sizeof *to_cache; i++) tidied_files += move_file(game, to_cache[i], cache);
    tidied_files += move_pcache(game, cache);
    {   /* (what a write cut short left of the old pipeline cache: a .tmp, never read) */
        char pattern[MAX_PATH];
        snprintf(pattern, sizeof pattern, "%s%s*.tmp", game, old_pcache);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char tmp[MAX_PATH];
                if (strncmp(fd.cFileName, old_pcache, sizeof old_pcache - 1)) continue;
                snprintf(tmp, sizeof tmp, "%s%s", game, fd.cFileName);
                DeleteFileA(tmp);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    char path[MAX_PATH];   /* (the program cache's lock, made again beside it in cache/) */
    snprintf(path, sizeof path, "%shookshot_programs.bin.lock", game);
    if (exists(path)) DeleteFileA(path);
    for (size_t i = 0; i < sizeof made_again / sizeof *made_again; i++) {
        snprintf(path, sizeof path, "%s%s", game, made_again[i]);
        if (exists(path)) DeleteFileA(path);
    }
}

/* the game folder's settings into the data folder (game: the folder, with its separator) */
static void move_settings(const char *game) {
    char data[MAX_PATH];
    snprintf(data, sizeof data, "%s" SEP_S, data_dir);
    for (size_t i = 0; i < sizeof to_data / sizeof *to_data; i++) moved_files += move_file(game, to_data[i], data);
}

/* the game folder's saves and settings of a build from before, into the data folder (game: the folder, with its separator) */
static void migrate(const char *game) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%suser", game);
    char shared_saves[MAX_PATH], own_saves[MAX_PATH];
    snprintf(shared_saves, sizeof shared_saves, "%s" SEP_S "save", data_dir);
    snprintf(own_saves, sizeof own_saves, "%s" SEP_S "save", path);
    const int both = is_dir(shared_saves) && is_dir(own_saves);
    if (move_saves(path) != data_dir) {   /* (the saves stay in <game>/user: the settings stay with them, the data folder too. A
                                             second game folder's saves meet the first's: it keeps its own saves and settings) */
        if (both) {
            /* (saves in both places: this game folder's are used, the data folder's are not, and a player who does not know
               why sees the wrong ones: an older build started in this folder after the move made new ones here, or another
               game folder moved its own to the data folder first. Neither is picked over the other or merged: a note says
               where both are and how to choose) */
            saves_twice = 1;
            char note[MAX_PATH];
            snprintf(note, sizeof note, "%ssaves-in-two-places.txt", game);
            if (!exists(note)) {
                FILE *f = fopen(note, "w");
                if (f) {
                    fprintf(f, "Hookshot found saves in two places:\n\n  %s (used by this game folder)\n  %s (not used by it)\n\n"
                               "This happens when an older version of Hookshot was started in this folder after its saves had "
                               "moved (it made new ones here), or when another game folder had moved its saves first.\n\n"
                               "To use the other saves instead: close the game, rename this folder's user folder (to user.old, "
                               "for example) and start the game again: it then uses the other saves, with the settings beside "
                               "them. To keep some slots from each, copy them with scripts/oot_saves.py (docs/saves.md). Nothing "
                               "was deleted.\n", own_saves, shared_saves);
                    fclose(f);
                }
            }
        }
        snprintf(data_dir, sizeof data_dir, "%s", path), hos_user_dir = data_dir;
    }
    move_settings(game);
    if (moved_files || saves_moved) {   /* (a note where they went, for a player who looks there: what moved, and only that) */
        snprintf(path, sizeof path, "%sfiles-moved.txt", game);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "Hookshot's %s now in\n%s\n\nThey moved there from this folder.%s%s The shader caches are in this folder's "
                       "cache folder (it can be deleted: the next start makes them again), the logs in its logs folder.\n",
                    saves_moved && moved_files ? "saves and settings are" : saves_moved ? "saves are" : "settings are", data_dir,
                    saves_kept ? " The saves stay in this folder's user folder (one of them could not move), with the settings."
                               : "",
                    set_aside[0] ? " The user folder was copied whole, but some of it could not be deleted after: what is left is "
                                   "in user.moved, which can be deleted once the game shows your saves." : "");
            fclose(f);
        }
    }
}

/* the platform's data folder into data_dir: 0 when there is none */
static int platform_dir(void) {
#ifdef _WIN32
    wchar_t w[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", w, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    char base[MAX_PATH * 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, base, (int)sizeof base, NULL, NULL)) return 0;
    if (snprintf(data_dir, sizeof data_dir, "%s\\Hookshot", base) >= (int)sizeof data_dir) return 0;
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char base[MAX_PATH];
    if (xdg && xdg[0] == '/') snprintf(base, sizeof base, "%s", xdg);
    else if (home && home[0] == '/') {
        snprintf(base, sizeof base, "%s/.local", home);
        CreateDirectoryA(base, NULL);
        snprintf(base, sizeof base, "%s/.local/share", home);
    } else return 0;
    CreateDirectoryA(base, NULL);
    if (snprintf(data_dir, sizeof data_dir, "%s/hookshot", base) >= (int)sizeof data_dir) return 0;
#endif
    CreateDirectoryA(data_dir, NULL);
    return is_dir(data_dir);
}

/* data_dir as an absolute path (a relative --user or "user" would move with the current folder) */
static void make_absolute(void) {
    char full[MAX_PATH];
#ifdef _WIN32
    if (_fullpath(full, data_dir, sizeof full)) snprintf(data_dir, sizeof data_dir, "%s", full);
#else
    char real[PATH_MAX];
    if (realpath(data_dir, real) && strlen(real) < sizeof full) snprintf(data_dir, sizeof data_dir, "%s", real);
#endif
    size_t n = strlen(data_dir);
    while (n > 1 && (data_dir[n - 1] == '/' || data_dir[n - 1] == '\\') && !(n == 3 && data_dir[1] == ':')) data_dir[--n] = 0;
}

int hos_game_file(char *out, size_t cap, const char *name) {
    char path[MAX_PATH];
    const DWORD n = hos_game_path_a(path, MAX_PATH);
    if (!n || n >= MAX_PATH || !cap) return 0;
    char *slash = strrchr(path, SEP);
#ifdef _WIN32
    if (!slash) slash = strrchr(path, '/');
#endif
    if (!slash) return 0;
    slash[1] = 0;
    const int len = snprintf(out, cap, "%s%s", path, name);
#ifdef _WIN32
    for (char *p = out + strlen(path); *p; p++)
        if (*p == '/') *p = SEP;
#endif
    return len >= 0 && len < (int)cap;
}

void hos_data_init(int argc, char **argv) {
    if (data_ready) return;
    data_ready = 1;
    const char *user = NULL;
    int image = 0;
    for (int i = 1; i < argc; i++) {   /* (hos_main.c's arguments) */
        if (!strcmp(argv[i], "--romfs") && i + 1 < argc) i++;
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "--trace") || !strcmp(argv[i], "--prepare-shaders")) continue;
        else image = 1;
    }
    char game_image[MAX_PATH], marker[MAX_PATH];
    const int builder_folder = !image && hos_game_file(game_image, sizeof game_image, "image.bin") && exists(game_image);
    /* (a portable install, #2367's Builder: .hookshot_builder_home in the game folder. Its data folder is the game folder's
       user/, so the saves go with the drive the game is on; they are there already, so nothing moves but the settings) */
    portable = builder_folder && hos_game_file(marker, sizeof marker, ".hookshot_builder_home") && exists(marker);
    int move = 0;
    if (user && *user) snprintf(data_dir, sizeof data_dir, "%s", user);
    else if (portable) hos_game_file(data_dir, sizeof data_dir, "user"), move = 2;
    else if (builder_folder && platform_dir()) move = 1;
    else if (builder_folder) hos_game_file(data_dir, sizeof data_dir, "user");   /* (no platform folder: the game's, as before) */
    else snprintf(data_dir, sizeof data_dir, "user");
    CreateDirectoryA(data_dir, NULL);
    make_absolute();
    hos_user_dir = data_dir;
    char game[MAX_PATH], sub[MAX_PATH];
    if (hos_game_file(game, sizeof game, "")) {
        if (hos_game_file(sub, sizeof sub, "cache")) CreateDirectoryA(sub, NULL);
        if (hos_game_file(sub, sizeof sub, "logs")) CreateDirectoryA(sub, NULL);
        tidy_game(game);
        if (move == 1) migrate(game);
        else if (move == 2) move_settings(game);
        /* (a settings file beside the game that the game does not read: the options never are; hookshot_env.txt is when the
           data folder has none. Players were told to put them there: the log says so) */
        char beside[MAX_PATH], mine[MAX_PATH];
        if (hos_game_file(beside, sizeof beside, "hookshot_options.ini") && exists(beside))
            snprintf(unread_options, sizeof unread_options, "%s", beside);
        if (hos_game_file(beside, sizeof beside, "hookshot_env.txt") && exists(beside) &&
            hos_data_path(mine, sizeof mine, "hookshot_env.txt") && exists(mine))
            snprintf(unread_env, sizeof unread_env, "%s", beside);
    }
}

const char *hos_data_dir(void) { return data_dir; }

int hos_data_path(char *out, size_t cap, const char *name) {
    if (!cap) return 0;
    const int n = snprintf(out, cap, "%s" SEP_S "%s", data_dir, name);
#ifdef _WIN32
    for (char *p = out + strlen(data_dir); *p; p++)
        if (*p == '/') *p = SEP;
#endif
    return n >= 0 && n < (int)cap;
}

#ifdef _WIN32
const wchar_t *hos_data_dir_w(void) {
    static wchar_t w[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, data_dir, -1, w, MAX_PATH)) w[0] = 0;
    return w;
}
#endif

void hos_data_log_migration(void) {
    hos_log("data: %s%s", data_dir, portable ? " (a portable install, .hookshot_builder_home: the game folder's user)" : "");
    if (saves_moved || saves_kept || saves_link || leftover) {
        if (saves_link == 2)
            hos_log("data: %s is a link (the player's own place for the saves): used as it is, the settings with them", saves_from);
        else if (saves_link)
            hos_log("data: %s holds a link (to a folder elsewhere): it is left as it is, the saves and settings stay in %s",
                    saves_from, saves_from);
        else if (saves_kept)
            hos_log("data: %d of %s's entries could not move: the saves stay in %s, the settings with them", saves_kept, saves_from,
                    saves_from);
        else if (leftover == 1)
            hos_log("data: %s held only what an earlier move of the saves could not delete: set aside as %s; the saves are in %s",
                    saves_from, set_aside, data_dir);
        else if (leftover == 2)
            hos_log("data: %s holds only what an earlier move of the saves could not delete, and is still in use: left as it is, "
                    "the saves are in %s", saves_from, data_dir);
        else {
            hos_log("data: the saves moved from %s", saves_from);
            if (set_aside[0])
                hos_log("data: %s could not be deleted whole after its copy: what is left is set aside as %s", saves_from,
                        set_aside);
        }
        if (saves_twice)
            hos_log("data: saves in two places: this game folder's (used) and the shared data folder's (not used): see "
                    "saves-in-two-places.txt in the game folder");
    }
    if (moved_files) hos_log("data: %d settings files moved from the game folder", moved_files);
    if (tidied_files) hos_log("data: %d logs and shader caches moved into the game folder's logs and cache folders", tidied_files);
    if (kept_files)
        hos_log("data: %d files stayed in the game folder (one there already, or no room): %s, ...", kept_files, kept_first);
    char mine[MAX_PATH];
    if (unread_options[0] && hos_data_path(mine, sizeof mine, "hookshot_options.ini"))
        hos_log("data: %s is not read: the game's settings are %s", unread_options, mine);
    if (unread_env[0] && hos_data_path(mine, sizeof mine, "hookshot_env.txt"))
        hos_log("data: %s is not read: the switches are %s's", unread_env, mine);
}
