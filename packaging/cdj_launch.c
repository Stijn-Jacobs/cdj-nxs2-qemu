/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * CDJ-Emulator.exe: the packaged program's entry point on Windows. It runs the
 * bundled Python on the launcher (app/launcher) with the same arguments:
 *
 *   runtime\python\python.exe -m launcher [args]      with PYTHONPATH=app
 *
 * A native launcher rather than a frozen Python, so the bundled scripts (the
 * relay, the DSP code generator QEMU starts, the firmware tools) run on a
 * plain interpreter. Built by packaging/package.py.
 */
#include <windows.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    wchar_t dir[MAX_PATH], python[MAX_PATH], app[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n == MAX_PATH)
        return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    _snwprintf(python, MAX_PATH, L"%ls\\runtime\\python\\python.exe", dir);
    _snwprintf(app, MAX_PATH, L"%ls\\app", dir);
    SetEnvironmentVariableW(L"PYTHONPATH", app);
    /* The bundled Python must not pick up a user's site-packages or startup
       file: the launcher runs only on what the package carries. */
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    SetEnvironmentVariableW(L"PYTHONSTARTUP", NULL);

    size_t len = wcslen(python) + 32;
    for (int i = 1; i < argc; i++)
        len += wcslen(argv[i]) * 2 + 3;
    wchar_t *cmd = HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
    if (!cmd)
        return 1;
    wchar_t *p = cmd + _snwprintf(cmd, len, L"\"%ls\" -m launcher", python);
    for (int i = 1; i < argc; i++) {
        /* CommandLineToArgvW quoting: backslashes double only before a quote. */
        *p++ = L' ';
        *p++ = L'"';
        for (const wchar_t *a = argv[i];; a++) {
            unsigned bs = 0;
            while (*a == L'\\') {
                a++;
                bs++;
            }
            if (!*a) {
                while (bs--) {
                    *p++ = L'\\';
                    *p++ = L'\\';
                }
                break;
            }
            if (*a == L'"')
                bs = bs * 2 + 1;
            while (bs--)
                *p++ = L'\\';
            *p++ = *a;
        }
        *p++ = L'"';
    }
    *p = 0;

    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(python, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxW(NULL, L"runtime\\python\\python.exe is missing: unzip the whole folder, not just this file.",
                    L"CDJ-Emulator", MB_ICONERROR);
        return 1;
    }
    /* Ctrl-C reaches the launcher itself, which stops the decks in order. */
    SetConsoleCtrlHandler(NULL, TRUE);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    return (int)rc;
}
