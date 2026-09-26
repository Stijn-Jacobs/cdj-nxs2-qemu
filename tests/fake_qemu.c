/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A stand-in for both QEMUs, for tests/test_launcher_rig.py. It records the
 * command line and the environment it was started with -- exactly as a native
 * program receives them, after any MSYS2 path conversion -- to
 * $FAKE_QEMU_OUT/<main|gui>-<tag>.txt, creates the SPI link socket's path
 * when it is the listening end (the launch scripts wait for it to appear),
 * and exits -- or, with FAKE_QEMU_HOLD=<seconds>, stays up that long, for
 * tests that stop a running rig.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(int argc, char **argv, char **envp)
{
    const char *out = NULL, *path = NULL, *hold = NULL;
    int server = 0, main_board = 0;
    char tag[256] = "unknown";

    for (char **e = envp; *e; e++)
        if (!strncmp(*e, "FAKE_QEMU_OUT=", 14))
            out = *e + 14;
        else if (!strncmp(*e, "FAKE_QEMU_HOLD=", 15))
            hold = *e + 15;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "cdj2000nxs2"))
            main_board = 1;
        const char *p = strstr(argv[i], "id=spilink,path=");
        if (p) {
            path = p + 16;
            server = strstr(argv[i], "server=on") != NULL;
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            if (!strncmp(base, "cdj-", 4)) {
                snprintf(tag, sizeof tag, "%s", base + 4);
                char *end = strstr(tag, "-spi.sock");
                if (end)
                    *end = 0;
            }
        }
    }
    if (!out)
        return 2;
    char name[4096];
    snprintf(name, sizeof name, "%s/%s-%s.txt", out, main_board ? "main" : "gui", tag);
    FILE *f = fopen(name, "wb");
    if (!f)
        return 3;
    for (int i = 0; i < argc; i++)
        fprintf(f, "A\t%s\n", argv[i]);
    for (char **e = envp; *e; e++)
        fprintf(f, "E\t%s\n", *e);
    fclose(f);
    if (server && path) {
        char sock[4096];
        snprintf(sock, sizeof sock, "%s", path);
        char *comma = strchr(sock, ',');
        if (comma)
            *comma = 0;
        f = fopen(sock, "wb");
        if (f)
            fclose(f);
    }
    if (hold) {
#ifdef _WIN32
        Sleep(atoi(hold) * 1000);
#else
        sleep(atoi(hold));
#endif
    }
    return 0;
}
