#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define JOBS 3 // Number of concurrent worker processes

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 8;
    int core_count = argc > 2 ? atoi(argv[2]) : 1;
    const char *names[JOBS] = {"IDLE", "NORMAL", "HIGH"};
    DWORD classes[JOBS] = {
        IDLE_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, HIGH_PRIORITY_CLASS // Windows priority classes
    };
    PROCESS_INFORMATION pi[JOBS];
    LARGE_INTEGER qpc_start[JOBS], qpc_end, frequency;
    FILETIME creation, exit_time, kernel, user;
    DWORD_PTR affinity_mask = 0;

    if (seconds < 1 || core_count < 1) {
        fprintf(stderr, "Usage: windows_launcher.exe [seconds=8] [cores=1]\n");
        return 1;
    }
    if (!QueryPerformanceFrequency(&frequency)) { // Query high-precision timer frequency
        fprintf(stderr, "QueryPerformanceFrequency failed.\n");
        return 1;
    }

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (core_count > (int)info.dwNumberOfProcessors)
        core_count = (int)info.dwNumberOfProcessors;
    for (int i = 0; i < core_count && i < (int)(sizeof(DWORD_PTR) * 8); ++i)
        affinity_mask |= ((DWORD_PTR)1 << i); // Build processor affinity bitmask

    ZeroMemory(pi, sizeof pi);
    for (int i = 0; i < JOBS; ++i) {
        STARTUPINFOA si;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        char command[256];
        snprintf(command, sizeof command, "windows_worker.exe cpu %d", seconds);

        QueryPerformanceCounter(&qpc_start[i]); // Record start performance tick
        if (!CreateProcessA(NULL, command, NULL, NULL, FALSE,
                            classes[i], NULL, NULL, &si, &pi[i])) { // Launch worker with priority class
            fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
            continue;
        }
        if (!SetProcessAffinityMask(pi[i].hProcess, affinity_mask)) // Bind worker to CPU affinity mask
            fprintf(stderr, "SetProcessAffinityMask failed: %lu\n", GetLastError());
    }

    printf("priority,pid,user_cpu_s,kernel_cpu_s,elapsed_s,exit_code\n");
    for (int i = 0; i < JOBS; ++i) {
        if (!pi[i].hProcess) continue;
        WaitForSingleObject(pi[i].hProcess, INFINITE); // Await worker process completion
        QueryPerformanceCounter(&qpc_end); // Record completion performance tick
        DWORD code = 0;
        GetExitCodeProcess(pi[i].hProcess, &code);
        if (GetProcessTimes(pi[i].hProcess, &creation, &exit_time, &kernel, &user)) { // Fetch kernel and user CPU time
            ULARGE_INTEGER u, k;
            u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
            k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
            double elapsed = (double)(qpc_end.QuadPart - qpc_start[i].QuadPart) /
                             (double)frequency.QuadPart;
            printf("%s,%lu,%.6f,%.6f,%.6f,%lu\n",
                   names[i], pi[i].dwProcessId, u.QuadPart / 1e7,
                   k.QuadPart / 1e7, elapsed, code);
        }
        CloseHandle(pi[i].hThread);
        CloseHandle(pi[i].hProcess);
    }
    return 0;
}
