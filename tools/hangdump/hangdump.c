/* hangdump <pid>: call stacks of every thread of a running (e.g. hung) process, as
 * module+offset lines, for tools/hangdump/symbolize.py. Each thread is suspended while its stack
 * is walked (dbghelp StackWalk64 with the target's unwind tables), then resumed.
 * Build (MSYS2 CLANG64): clang -O2 hangdump.c -o hangdump.exe -ldbghelp -lpsapi */
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

static void module_of(HANDLE process, DWORD64 address, char* out, size_t size) {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
            MODULEINFO info;
            if (!GetModuleInformation(process, modules[i], &info, sizeof(info))) continue;
            const DWORD64 base = (DWORD64)info.lpBaseOfDll;
            if (address >= base && address < base + info.SizeOfImage) {
                char name[MAX_PATH] = "?";
                GetModuleBaseNameA(process, modules[i], name, sizeof(name));
                snprintf(out, size, "%s+0x%llx", name, (unsigned long long)(address - base));
                return;
            }
        }
    }
    snprintf(out, size, "?+0x%llx", (unsigned long long)address);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: hangdump <pid>\n");
        return 2;
    }
    const DWORD pid = (DWORD)strtoul(argv[1], NULL, 10);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (!thread) continue;
        wchar_t* description = NULL;
        char name[256] = "";
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), NULL, NULL);
            LocalFree(description);
        }
        FILETIME created, exited, kernel, user;
        GetThreadTimes(thread, &created, &exited, &kernel, &user);
        const double cpu = ((ULARGE_INTEGER){.LowPart = user.dwLowDateTime, .HighPart = user.dwHighDateTime}.QuadPart +
                            (ULARGE_INTEGER){.LowPart = kernel.dwLowDateTime, .HighPart = kernel.dwHighDateTime}.QuadPart) / 1e7;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        CONTEXT context = {.ContextFlags = CONTEXT_FULL};
        if (GetThreadContext(thread, &context)) {
            printf("thread %lu \"%s\" cpu %.1f s\n", entry.th32ThreadID, name, cpu);
            STACKFRAME64 frame = {0};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            for (int depth = 0; depth < 40; ++depth) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, NULL,
                                 SymFunctionTableAccess64, SymGetModuleBase64, NULL) ||
                    !frame.AddrPC.Offset) {
                    break;
                }
                char where[MAX_PATH + 32];
                module_of(process, frame.AddrPC.Offset, where, sizeof(where));
                printf("  %s\n", where);
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    SymCleanup(process);
    return 0;
}
