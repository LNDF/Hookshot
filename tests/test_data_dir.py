"""The data folder (host/hos_data.c): which folder a run keeps its saves and settings in, the game folder's cache/ and logs/,
and the one-time move of what a build from before kept loose in the game folder.

host/hos_data.c is built with the Linux stand-ins (host/posix/win32_posix.c) into a small program that prints the folder it
chose and the saves' folder. The program is copied into game folders built in a temporary folder, as the builder makes them
(image.bin beside it, or the game in bin/ beside image.bin), and run there:

- every run: the game folder's loose caches go into its cache/ (the play files by their prefix too) and its logs into its logs/;
- --user <folder> wins, and no setting or save moves; a run given its image.bin uses "user" in the current folder;
- a builder's folder uses $XDG_DATA_HOME/hookshot; the settings go there, the saves from <game>/user, and a note is left;
  language.txt (the builder's) stays, and the prompts made from the player's controls are deleted;
- a file already where it goes is not written over: the game folder's copy stays where it is;
- when a save entry cannot move (one of that name is there already, as for a second game folder beside the first), the
  saves stay in <game>/user together, and the data folder with them: the settings move there, not to the shared folder;
- a relative link in the saves, on the same drive, is never renamed into the data folder, where it would name nothing: it is
  made again there to the same folder (Linux; #2282, whose own cases tests/test_linux_saves_move.py runs on hos_data.c);
- saves in both places (a downgrade, then the upgrade again): this folder's are kept and used, nothing merged, and
  saves-in-two-places.txt says where both are and how to use the others;
- a settings file beside the game the game does not read is said in the log;
- a portable install (.hookshot_builder_home): the data folder is <game>/user, and nothing goes to the per-user folder;
- the game in bin/: the files come from the folder above it, the one with image.bin;
- across file systems (the data folder on another drive, where a rename fails: /dev/shm against /tmp here), the saves are
  copied, every one, and only then the originals deleted; an entry that cannot be copied deletes the copies made (the
  originals untouched, whole); what a move could not delete is never taken for the saves by a later start, held or not; an entry that cannot be copied brings back the ones that went (all or none); a link is never copied
  through: it is made again in the data folder (Linux), and the folder it points to is untouched; an original that cannot be deleted after its
  copy leaves <game>/user renamed user.moved, so the next start keeps the data folder.

python -I -B tests/test_data_dir.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "builds host/hos_data.c with the Linux stand-ins (host/posix); the Windows branch is the platform folder alone"}

ROOT = Path(__file__).resolve().parents[1]

MAIN = r"""
#include <stdarg.h>
#include <stdio.h>
void hos_data_init(int argc, char **argv);
const char *hos_data_dir(void);
void hos_data_log_migration(void);
const char *hos_user_dir;   /* (host/nn_fs.c) */
void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("log: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
int main(int argc, char **argv) {
    hos_data_init(argc, argv);
    hos_data_log_migration();
    printf("data=%s\nuser=%s\n", hos_data_dir(), hos_user_dir);
    return 0;
}
"""


def run(exe, args, cwd, xdg):
    env = dict(os.environ, XDG_DATA_HOME=str(xdg))
    out = subprocess.run([str(exe)] + args, cwd=cwd, env=env, check=True, capture_output=True, text=True).stdout
    values = dict(line.split("=", 1) for line in out.splitlines() if "=" in line and not line.startswith("log:"))
    return Path(values["data"]), Path(values["user"]), out


def game_folder(root, built, in_bin=False):
    """A builder's folder: image.bin, the game (in bin/ when asked), and what a build from before left beside it"""
    root.mkdir(parents=True)
    (root / "image.bin").write_bytes(b"image")
    where = root / "bin" if in_bin else root
    where.mkdir(exist_ok=True)
    exe = where / "hookshot"
    shutil.copy2(built, exe)
    files = {
        "hookshot_options.ini": "vsync=1\n", "hookshot_env.txt": "RX_VK_X=1\n", "hookshot_log.txt": "log\n",
        "hookshot_log.1.txt": "log 1\n", "hookshot_startup.log": "stderr\n", "language.txt": "fr\n", "mkw_vk_pipelines.bin": "driver",
        "mkw_vk_pipelines_play_3.bin": "play", "hookshot_programs.bin": "programs", "hookshot_programs.idx": "index",
        "hookshot_programs.bin.lock": "", "hookshot_pipelines.bin": "notes", "hookshot_prepared.bin": "prepared",
        "hookshot_shaders.bin": "seed", "hookshot_pipeline_states_shipped.bin": "shipped",
        "hookshot_pipelines.bin.running": "1\n", "mkw_vk_pipelines_play_4.bin.tmp": "cut short",
    }
    for name, text in files.items():
        (root / name).write_text(text)
    (root / "prompts").mkdir()
    (root / "prompts" / "Common.key").write_text("k")
    (root / "prompts" / "Common.bntx").write_text("game's")
    (root / "user" / "save" / "0100").mkdir(parents=True)
    (root / "user" / "save" / "0100" / "file").write_text("my save")
    (root / "user" / "mouse_sens.txt").write_text("1.5\n")
    return exe


def other_fs(tmp, built):
    """The cross-drive cases: the data folder on another file system than the game folder, so every save is a copy and delete"""
    shm = Path("/dev/shm")
    if not shm.is_dir() or not os.access(shm, os.W_OK) or os.stat(shm).st_dev == os.stat(tmp).st_dev:
        print("cross-file-system cases skipped: no /dev/shm on another file system than %s" % tmp)
        return
    as_root = hasattr(os, "geteuid") and os.geteuid() == 0   # (permissions do not stop root: those cases need another user)
    with tempfile.TemporaryDirectory(prefix="hookshot-data-dir-", dir=shm) as other:
        other = Path(other)

        # a plain copy and delete: everything arrives, <game>/user is gone
        game = tmp / "e" / "game"
        exe = game_folder(game, built)
        data, user, out = run(exe, [], tmp / "e", other / "e")
        assert user == data == other / "e" / "hookshot", (data, user, out)
        assert (data / "save" / "0100" / "file").read_text() == "my save" and not (game / "user").exists(), out

        # a link (here a symbolic link to an emulator's folder) is never copied through or deleted into: made again in the data
        # folder, to the same folder, whose files stay (#2282)
        game = tmp / "f" / "game"
        exe = game_folder(game, built)
        emulator = tmp / "f" / "emulator-saves"
        shutil.move(str(game / "user" / "save"), str(emulator))
        os.symlink(emulator, game / "user" / "save")
        data, user, out = run(exe, [], tmp / "f", other / "f")
        assert user == data == other / "f" / "hookshot", (user, out)
        assert (emulator / "0100" / "file").read_text() == "my save", out
        assert (data / "save").is_symlink() and (data / "save").resolve() == emulator.resolve(), out
        assert (data / "mouse_sens.txt").exists() and not (game / "user").exists(), out

        if as_root:
            print("cross-file-system permission cases skipped: run as root")
            return

        # one entry that cannot be copied (unreadable): the one that went comes back, and the saves stay whole in <game>/user
        game = tmp / "g" / "game"
        exe = game_folder(game, built)
        (game / "user" / "present.txt").write_text("secret")
        os.chmod(game / "user" / "present.txt", 0)
        try:
            data, user, out = run(exe, [], tmp / "g", other / "g")
        finally:
            os.chmod(game / "user" / "present.txt", 0o600)
        assert user == data == game / "user", (user, out)   # (the settings stay with the saves)
        assert (game / "user" / "save" / "0100" / "file").read_text() == "my save" and (game / "user" / "mouse_sens.txt").exists()
        shared = other / "g" / "hookshot"
        assert not (shared / "save").exists() and not (shared / "mouse_sens.txt").exists(), out
        assert "the saves stay in" in out, out

        # copied whole, but the original cannot be deleted (a read-only folder): <game>/user is set aside as user.moved, and the
        # next start keeps the data folder rather than going back to what was left
        game = tmp / "h" / "game"
        exe = game_folder(game, built)
        os.chmod(game / "user" / "save" / "0100", 0o555)
        try:
            data, user, out = run(exe, [], tmp / "h", other / "h")
            assert user == data == other / "h" / "hookshot", (user, out)
            assert (data / "save" / "0100" / "file").read_text() == "my save"
            assert not (game / "user").exists() and (game / "user.moved" / "save" / "0100" / "file").exists(), out
            assert "set aside as" in out and "user.moved" in (game / "files-moved.txt").read_text(), out
            _, user2, out = run(exe, [], tmp / "h", other / "h")
            assert user2 == data, (user2, out)
        finally:
            for d in (game / "user.moved" / "save" / "0100", game / "user" / "save" / "0100"):
                if d.exists():
                    os.chmod(d, 0o755)

        # (the native Windows report on #2317, cases 1 and 2) an entry whose original cannot be deleted whole, and a later one that
        # cannot be copied: no original is deleted before every entry has gone, so the copies made are deleted and <game>/user
        # stays as it was, whole, whatever order the entries are listed in. Nothing of it in the data folder
        game = tmp / "r" / "game"
        exe = game_folder(game, built)
        # (z-tree made first: tmpfs lists the newest first, so a-tree is tried first, as in the report; the result must not
        # depend on the order, and with z-tree first there is nothing to undo)
        files = ("z-tree/one", "z-tree/two", "a-tree/one", "a-tree/held/two")
        for f in files:
            (game / "user" / f).parent.mkdir(parents=True, exist_ok=True)
            (game / "user" / f).write_text(f)
        os.chmod(game / "user" / "a-tree" / "held", 0o555)   # (a-tree/one can be deleted, held/two cannot: a partial original)
        os.chmod(game / "user" / "z-tree" / "two", 0)   # (cannot be read: its copy fails)
        try:
            data, user, out = run(exe, [], tmp / "r", other / "r")
        finally:
            os.chmod(game / "user" / "z-tree" / "two", 0o644)
            os.chmod(game / "user" / "a-tree" / "held", 0o755)
        assert user == data == game / "user", (user, out)
        for f in files:
            assert (game / "user" / f).read_text() == f, (f, out)
        shared = other / "r" / "hookshot"
        assert not any((shared / n).exists() for n in ("a-tree", "z-tree", "save", "mouse_sens.txt")), out
        assert (game / "user" / "save" / "0100" / "file").read_text() == "my save"

        # (case 3) copied whole, an original that cannot be deleted whole, and <game>/user that cannot be set aside (the game
        # folder read-only): the first start uses the whole copy; a later start does not take what was left for the saves, still
        # held or not, and sets it aside once it can
        game = tmp / "s" / "game"
        exe = game_folder(game, built)
        (game / "user" / "a-tree").mkdir()
        for f in ("one", "two"):
            (game / "user" / "a-tree" / f).write_text("a-tree/" + f)
        os.chmod(game / "user" / "a-tree", 0o555)
        shared = other / "s" / "hookshot"
        try:
            os.chmod(game, 0o555)   # (user/ cannot be renamed)
            try:
                data, user, out = run(exe, [], tmp / "s", other / "s")
                assert user == data == shared, (user, out)
                assert (shared / "a-tree" / "one").exists() and (shared / "save" / "0100" / "file").read_text() == "my save"
                assert (game / "user" / "a-tree").exists() and not (game / "user.moved").exists(), out
                _, user2, out = run(exe, [], tmp / "s", other / "s")   # (still held: left as it is, the data folder used)
                assert user2 == shared and "still in use" in out, (user2, out)
            finally:
                os.chmod(game, 0o755)
            _, user3, out = run(exe, [], tmp / "s", other / "s")   # (released: set aside, the data folder used)
            assert user3 == shared and not (game / "user").exists() and (game / "user.moved" / "a-tree").exists(), (user3, out)
            assert "could not delete: set aside" in out, out
        finally:
            for d in (game / "user" / "a-tree", game / "user.moved" / "a-tree"):
                if d.exists():
                    os.chmod(d, 0o755)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-data-dir-") as tmp:
        tmp = Path(tmp)
        main_c = tmp / "main.c"
        main_c.write_text(MAIN)
        built = tmp / "data_dir"
        subprocess.run([a.cc, "-std=gnu11", "-I", str(ROOT / "host" / "posix"), "-I", str(ROOT / "host"),
                        str(ROOT / "host" / "hos_data.c"), str(ROOT / "host" / "posix" / "win32_posix.c"), str(main_c),
                        "-o", str(built), "-lpthread", "-ldl"], check=True)

        CACHES = ("hookshot_vk_pipelines.bin", "hookshot_vk_pipelines_play_3.bin", "hookshot_programs.bin", "hookshot_programs.idx",
                  "hookshot_pipelines.bin", "hookshot_prepared.bin", "hookshot_pipelines.bin.running")   # (the Vulkan pipeline cache renamed from mkw_vk_pipelines*)
        LOGS = ("hookshot_log.txt", "hookshot_log.1.txt", "hookshot_startup.log")
        SETTINGS = ("hookshot_options.ini", "hookshot_env.txt")

        def tidied(game):
            """the game folder's caches in its cache/, its logs in its logs/, none left loose"""
            for name in CACHES:
                assert (game / "cache" / name).exists() and not (game / name).exists(), name
            assert not list(game.glob("mkw_vk_pipelines*"))   # (the .tmp of a write cut short too)
            for name in LOGS:
                assert (game / "logs" / name).exists() and not (game / name).exists(), name
            assert not (game / "hookshot_programs.bin.lock").exists()
            for name in ("hookshot_shaders.bin", "hookshot_pipeline_states_shipped.bin", "image.bin", "language.txt"):
                assert (game / name).exists(), name   # (the build's own stay where the build put them)

        # --user wins, made absolute; the caches and logs go into the game folder's cache/ and logs/, settings and saves stay
        game = tmp / "a" / "game"
        exe = game_folder(game, built)
        data, user, out = run(exe, ["--user", "mine"], tmp / "a", tmp / "a" / "xdg")
        assert data == tmp / "a" / "mine" and user == data, (data, user)
        tidied(game)
        assert "into the game folder's logs and cache folders" in out, out
        for name in SETTINGS:
            assert (game / name).exists() and not (data / name).exists(), name
        assert (game / "user" / "save").is_dir() and not (tmp / "a" / "xdg").exists()

        # a development run, given its image.bin: "user" in the current folder
        data, user, out = run(exe, [str(game / "image.bin")], tmp / "a", tmp / "a" / "xdg")
        assert data == tmp / "a" / "user" and user == data, (data, user)
        tidied(game)
        assert "moved" not in out, out

        # a builder's folder: settings and saves to $XDG_DATA_HOME/hookshot, caches and logs into the game folder's own
        game = tmp / "b" / "game"
        exe = game_folder(game, built)
        xdg = tmp / "b" / "xdg"
        data, user, out = run(exe, [], tmp / "b", xdg)
        assert data == xdg / "hookshot" and user == data, (data, user, out)
        tidied(game)
        for name in SETTINGS:
            assert (data / name).exists() and not (game / name).exists(), name
        for name in CACHES + LOGS:
            assert not (data / name).exists() and not (data / "shaders").exists(), name
        assert (data / "save" / "0100" / "file").read_text() == "my save" and (data / "mouse_sens.txt").exists()
        assert not (game / "user").exists()
        assert not (data / "language.txt").exists()
        assert not (game / "prompts" / "Common.key").exists() and (game / "prompts" / "Common.bntx").exists()
        note = (game / "files-moved.txt").read_text()
        assert "Hookshot's saves" in note and "cache folder" in note and "logs folder" in note, note
        assert "settings files moved" in out and "the saves moved" in out and "cache folders" in out, out
        # a second start: nothing left to move, the same folder
        data2, _, out = run(exe, [], tmp / "b", xdg)
        assert data2 == data and "moved" not in out, out

        # a second game folder (a nightly beside a release): its saves meet the first one's and stay in its user/; its settings
        # stay with them there, so it runs with its own of both and the first folder's settings are untouched. What is there
        # already is not written over
        game = tmp / "c" / "game"
        exe = game_folder(game, built)
        xdg = tmp / "c" / "xdg"
        (xdg / "hookshot" / "save").mkdir(parents=True)
        (xdg / "hookshot" / "hookshot_options.ini").write_text("mine\n")
        (game / "cache").mkdir(parents=True)
        (game / "cache" / "hookshot_programs.bin").write_text("newer")
        data, user, out = run(exe, [], tmp / "c", xdg)
        assert data == user == game / "user", (data, user, out)
        assert (game / "user" / "hookshot_options.ini").read_text() == "vsync=1\n" and not (game / "hookshot_options.ini").exists()
        assert (xdg / "hookshot" / "hookshot_options.ini").read_text() == "mine\n"
        assert (game / "user" / "save" / "0100" / "file").exists() and (game / "user" / "mouse_sens.txt").exists()
        assert (game / "cache" / "hookshot_programs.bin").read_text() == "newer" and (game / "hookshot_programs.bin").exists()
        assert (game / "cache" / "hookshot_vk_pipelines.bin").exists()
        assert "the saves stay in" in out and "the settings with them" in out and "stayed in the game folder" in out, out
        assert "saves in two places" in out and "user.old" in (game / "saves-in-two-places.txt").read_text(), out
        data2, user2, out = run(exe, [], tmp / "c", xdg)   # (every start: the same, its own saves and settings)
        assert data2 == user2 == game / "user" and "settings files moved" not in out, out

        # a downgrade, then the upgrade again: the saves moved to the data folder; an older build started in the same folder
        # made new ones in user/; the next start keeps those (both sets kept, nothing merged) and says where the others are.
        # Renaming user/ as the note says brings the moved saves back
        game = tmp / "dg" / "game"
        exe = game_folder(game, built)
        xdg = tmp / "dg" / "xdg"
        data, _, _ = run(exe, [], tmp / "dg", xdg)   # (the upgrade: the saves move)
        (game / "user" / "save" / "0100").mkdir(parents=True)   # (the older build's start: a fresh save beside the game)
        (game / "user" / "save" / "0100" / "file").write_text("new")
        data2, user2, out = run(exe, [], tmp / "dg", xdg)
        assert data2 == user2 == game / "user", (data2, user2, out)
        note = (game / "saves-in-two-places.txt").read_text()
        assert str(game / "user" / "save") in note and str(data / "save") in note and "saves in two places" in out, out
        assert (data / "save" / "0100" / "file").read_text() == "my save"   # (the moved saves are untouched)
        (game / "user").rename(game / "user.old")
        data3, user3, _ = run(exe, [], tmp / "dg", xdg)
        assert data3 == user3 == data and (data / "save" / "0100" / "file").read_text() == "my save"

        # a relative link (user/save -> a folder beside the game, synced elsewhere), on the same drive: never renamed into the
        # data folder, where it would name nothing, but made again there to the same folder (#2282), which is left untouched
        game = tmp / "l" / "game"
        exe = game_folder(game, built)
        synced = tmp / "l" / "synced-saves"
        shutil.move(str(game / "user" / "save"), str(synced))
        os.symlink(os.path.join("..", "..", "synced-saves"), game / "user" / "save")
        data, user, out = run(exe, [], tmp / "l", tmp / "l" / "xdg")
        assert data == user == tmp / "l" / "xdg" / "hookshot", (data, user, out)
        assert (data / "save").is_symlink() and (data / "save").resolve() == synced.resolve(), out
        assert (data / "save" / "0100" / "file").read_text() == "my save" and (synced / "0100" / "file").is_file()
        assert not (game / "user").exists(), out

        # a settings file beside the game the game does not read is said in the log: hookshot_env.txt when the data folder has
        # one, the options always
        game = tmp / "u" / "game"
        exe = game_folder(game, built)
        xdg = tmp / "u" / "xdg"
        run(exe, [], tmp / "u", xdg)   # (moves both into the data folder)
        (game / "hookshot_env.txt").write_text("RX_VK_X=2\n")
        (game / "hookshot_options.ini").write_text("vsync=0\n")
        (xdg / "hookshot" / "hookshot_options.ini").write_text("vsync=1\n")
        _, _, out = run(exe, [], tmp / "u", xdg)
        assert "hookshot_env.txt is not read" in out and "hookshot_options.ini is not read" in out, out

        # a portable install (.hookshot_builder_home, #2367): the data folder is the game folder's user/; the saves stay there,
        # only the settings move into it; nothing goes to the per-user folder
        game = tmp / "p" / "game"
        exe = game_folder(game, built)
        (game / ".hookshot_builder_home").write_text("../HookshotBuilder")
        xdg = tmp / "p" / "xdg"
        data, user, out = run(exe, [], tmp / "p", xdg)
        assert data == user == game / "user" and "portable install" in out, (data, user, out)
        assert (game / "user" / "save" / "0100" / "file").read_text() == "my save"
        assert (game / "user" / "hookshot_options.ini").exists() and not (game / "hookshot_options.ini").exists()
        assert not (xdg / "hookshot").exists() and not (game / "files-moved.txt").exists()
        tidied(game)

        # the game in bin/ beside image.bin (the launcher's layout): the folder above bin/ is the game folder
        game = tmp / "d" / "game"
        exe = game_folder(game, built, in_bin=True)
        xdg = tmp / "d" / "xdg"
        data, user, _ = run(exe, [], tmp / "d", xdg)
        tidied(game)
        assert not (game / "bin" / "cache").exists() and not (game / "bin" / "logs").exists()
        assert data == xdg / "hookshot" and (data / "hookshot_options.ini").exists() and not (game / "hookshot_options.ini").exists()
        assert (data / "save" / "0100" / "file").exists()

        other_fs(tmp, built)
    print("PASS: the caches and logs go into the game folder's cache/ and logs/ on every run; --user and a development run keep "
          "their folder; a builder's folder moves its settings and saves to the data folder once, writes nothing over, keeps the "
          "saves together, and finds the folder above bin/; across file systems the saves are copied all or none, never through "
          "a link, and an original left behind is set aside")


if __name__ == "__main__":
    main()
