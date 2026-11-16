#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: windows_worker.exe cpu|io|mixed seconds\n");
        return 1;
    }
    int seconds = atoi(argv[2]);
    if (seconds < 1) return 1;

    DWORD start = GetTickCount(); // Record baseline millisecond tick
    volatile unsigned long value = 1; // Volatile variable prevents compiler loop optimization

    // Run workload until elapsed milliseconds reaches specified duration in seconds
    while ((DWORD)(GetTickCount() - start) < (DWORD)seconds * 1000u) {
        if (!strcmp(argv[1], "io")) {
            Sleep(20); // 20ms sleep: simulates I/O blocking
        } else if (!strcmp(argv[1], "mixed")) {
            for (int i = 0; i < 80000; ++i) // Arithmetic computation burst
                value = value * 1664525u + 1013904223u;
            Sleep(10); // 10ms sleep
        } else {
            for (int i = 0; i < 300000; ++i) // Continuous CPU-intensive arithmetic loop
                value = value * 1664525u + 1013904223u;
        }
    }
    return (int)(value & 0u);
}
