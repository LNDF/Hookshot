"""Save migration must not commit a copy whose directory listing failed.

Linux game-free fault injection over the actual hos_data.c and POSIX shim.
The injected Win32-shaped error is not a native Windows observation.
python -I -B tests/test_data_copy_enumeration.py [--cc gcc] [--source-root PATH]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "Linux POSIX shim with deterministic directory-listing fault injection"}
ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''
#include "hos_paths.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static int armed, injected, late, faults, top;
static HANDLE listing;
static char source[MAX_PATH];
static BOOL migration_rename(LPCSTR from, LPCSTR to, DWORD flags) {
    if (!strncmp(from, source, strlen(source))) {
        armed = 1;
        SetLastError(17); /* ERROR_NOT_SAME_DEVICE */
        return FALSE;
    }
    return MoveFileExA(from, to, flags);
}
static HANDLE migration_first(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    /* (top: the move's own listing of the user folder, before anything moved) */
    if (top && !injected && !strncmp(pattern, source, strlen(source)) && !strcmp(pattern + strlen(source) + 1, "*")) {
        if (late) {
            HANDLE h = FindFirstFileA(pattern, fd);
            if (h == INVALID_HANDLE_VALUE) return h;
            while (!strcmp(fd->cFileName, ".") || !strcmp(fd->cFileName, ".."))
                if (!FindNextFileA(h, fd)) { FindClose(h); return INVALID_HANDLE_VALUE; }
            listing = h;
            return h;
        }
        injected = 1;
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (armed && faults && !injected && !strncmp(pattern, source, strlen(source))) {
        if (late) {
            HANDLE h = FindFirstFileA(pattern, fd);
            if (h == INVALID_HANDLE_VALUE) return h;
            while (!strcmp(fd->cFileName, ".") || !strcmp(fd->cFileName, ".."))
                if (!FindNextFileA(h, fd)) { FindClose(h); return INVALID_HANDLE_VALUE; }
            listing = h;
            return h;
        }
        injected = 1;
        SetLastError(faults == 2 ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstFileA(pattern, fd);
}
static BOOL migration_next(HANDLE h, WIN32_FIND_DATAA *fd) {
    if (late && !injected && h == listing) {
        injected = 1;
        SetLastError(faults == 2 ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return FindNextFileA(h, fd);
}
#define MoveFileExA migration_rename
#define FindFirstFileA migration_first
#define FindNextFileA migration_next
#include "hos_data.c"
#undef MoveFileExA
#undef FindFirstFileA
#undef FindNextFileA
const char *hos_user_dir;
void hos_log(const char *fmt, ...) { (void)fmt; }
int main(int argc, char **argv) {
    if (argc != 4) return 90;
    late = !strcmp(argv[3], "next") || !strcmp(argv[3], "list-next");
    top = !strncmp(argv[3], "list", 4);
    faults = !strcmp(argv[3], "normal") ? 0 : !strcmp(argv[3], "empty") ? 2 : 1;
    snprintf(source, sizeof source, "%s", argv[1]);
    snprintf(data_dir, sizeof data_dir, "%s", argv[2]);
    const char *selected = move_saves(source);
    printf("selected=%s\ninjected=%d\n", selected, injected);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--source-root", type=Path, default=ROOT)
    p.add_argument("--case", choices=("all", "first", "next", "normal", "empty", "list", "list-next"), default="all")
    a = p.parse_args()
    root = a.source_root.resolve()
    with tempfile.TemporaryDirectory(prefix="copy-enumeration-") as temporary:
        tmp = Path(temporary)
        driver, exe = tmp / "driver.c", tmp / "probe"
        driver.write_text(DRIVER)
        subprocess.run([a.cc, "-std=gnu11", "-I", str(root / "host/posix"),
                        "-I", str(root / "host"), str(driver),
                        str(root / "host/posix/win32_posix.c"), "-lpthread", "-ldl",
                        "-o", str(exe)], check=True)
        cases = ("first", "next", "normal", "empty", "list", "list-next") if a.case == "all" else (a.case,)
        for case in cases:
            source, destination = tmp / case / "user", tmp / case / "data"
            (source / "tree").mkdir(parents=True)
            destination.mkdir()
            payload = {} if case == "empty" else {"tree/one": b"synthetic one", "tree/two": b"synthetic two"}
            if case.startswith("list"):   # (two entries, so that a listing stopped after the first leaves one unseen)
                (source / "other").mkdir()
                payload["other/three"] = b"synthetic three"
            for name, value in payload.items():
                (source / name).write_bytes(value)
            r = subprocess.run([str(exe), str(source), str(destination), case], check=True,
                               capture_output=True, text=True, timeout=10)
            print("case=" + case + "\n" + r.stdout, end="")
            values = dict(line.split("=", 1) for line in r.stdout.splitlines())
            present = {str(p.relative_to(source)): p.read_bytes()
                       for p in source.rglob("*") if p.is_file()}
            copied = {str(p.relative_to(destination)): p.read_bytes()
                      for p in destination.rglob("*") if p.is_file()}
            print("source_payload=%r destination_payload=%r" % (present, copied))
            assert values["injected"] == ("0" if case == "normal" else "1"), "harness fault was not exercised"
            if case in ("list", "list-next"):   # (the user folder's own listing failed: nothing moves, nothing is recorded)
                assert Path(values["selected"]) == source and present == payload, (
                    "LISTING_FAILURE_SWITCHED_FOLDER: the saves must stay in use where they are", values, present, payload)
                assert copied == {}, ("nothing may move when the user folder was not listed whole", copied)
            elif case in ("first", "next"):
                assert Path(values["selected"]) == source and present == payload, (
                    "COPY_ENUMERATION_FAILURE_COMMITTED: active source must remain complete",
                    values, present, payload)
                assert not (destination / "tree").exists(), "failed copy must be removed"
            else:
                marker = copied.pop(".hookshot-moved", None)
                assert marker is not None, "successful move was not recorded"
                assert Path(values["selected"]) == destination and copied == payload, (values, copied)
                assert not source.exists() and (destination / "tree").is_dir(), "successful copy was not committed"
    print("PASS: copy-listing errors preserve the active source; complete and empty copies still migrate; a user folder "
          "that could not be listed whole stays in use, nothing moved")


if __name__ == "__main__":
    main()
