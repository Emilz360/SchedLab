#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <errno.h>

#define JOBS 4 // Number of benchmark worker processes

static pid_t children[JOBS]; // Process IDs of spawned workers
static volatile sig_atomic_t stopping = 0; // Flag set upon receiving termination signals
static volatile unsigned long work_sink; // Volatile sink to prevent compiler optimizing away arithmetic loops

// Job definition: name, workload kind (cpu/io/mixed), and nice value
typedef struct { const char *name, *kind; int nice_value; } Job;
static const Job jobs[JOBS] = {
    {"cpu_nice0", "cpu", 0}, // CPU-bound worker with default nice value (0)
    {"cpu_nice10", "cpu", 10}, // CPU-bound worker with lower priority nice value (10)
    {"io_nice0", "io", 0}, // I/O-bound worker yielding frequently via usleep
    {"mixed_nice0", "mixed", 0} // Mixed worker interleaving computation with sleep
};

// Signal handler to propagate termination signals to all active child workers
static void stop_handler(int sig) {
    (void)sig;
    stopping = 1;
    for (int i = 0; i < JOBS; ++i)
        if (children[i] > 0) kill(children[i], SIGTERM);
}

// Executes worker workload loop for the specified duration in seconds
static void run_worker(const char *kind, int seconds) {
    time_t end = time(NULL) + seconds;
    while (time(NULL) < end) {
        if (!strcmp(kind, "io")) {
            usleep(20000); // 20ms sleep: triggers voluntary context switches
        } else if (!strcmp(kind, "mixed")) {
            for (int i = 0; i < 80000; ++i) // Arithmetic computation burst
                work_sink = work_sink * 1664525u + 1013904223u;
            usleep(10000); // 10ms sleep
        } else {
            for (int i = 0; i < 300000; ++i) // Continuous CPU-intensive arithmetic loop
                work_sink = work_sink * 1664525u + 1013904223u;
        }
    }
}

// Main launcher: pins workers to CPU affinity mask, forks jobs, and prints rusage statistics
int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 8;
    int core_count = argc > 2 ? atoi(argv[2]) : 1;
    cpu_set_t allowed, selected;
    struct timespec starts[JOBS], ends[JOBS];

    if (seconds < 1 || core_count < 1) {
        fprintf(stderr, "Usage: %s [seconds=8] [number_of_cores=1]\n", argv[0]);
        return 1;
    }

    signal(SIGINT, stop_handler); // Catch Ctrl+C
    signal(SIGTERM, stop_handler); // Catch termination signal

    // Query allowed CPU affinity mask and select core_count CPUs
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed) == -1) {
        perror("sched_getaffinity");
        return 1;
    }
    CPU_ZERO(&selected);
    int chosen = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && chosen < core_count; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            CPU_SET(cpu, &selected);
            chosen++;
        }
    }
    if (chosen == 0) {
        fprintf(stderr, "No permitted CPUs found.\n");
        return 1;
    }
    if (chosen < core_count)
        fprintf(stderr, "Only %d CPUs are available to this process.\n", chosen);

    // Fork and launch child worker processes
    for (int i = 0; i < JOBS; ++i) {
        clock_gettime(CLOCK_MONOTONIC, &starts[i]); // Record start timestamp
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            stop_handler(SIGTERM);
            break;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL); // Restore default signals in child
            signal(SIGTERM, SIG_DFL);
            if (setpriority(PRIO_PROCESS, 0, jobs[i].nice_value) == -1) // Set CFS nice priority
                perror("setpriority");
            if (sched_setaffinity(0, sizeof selected, &selected) == -1) // Pin child to CPU core mask
                perror("sched_setaffinity");
            run_worker(jobs[i].kind, seconds);
            _exit(0);
        }
        children[i] = pid;
    }

    printf("worker,pid,nice,user_cpu_s,system_cpu_s,elapsed_s,voluntary_cs,involuntary_cs\n");

    // Collect and print performance statistics via wait4 and struct rusage
    for (int i = 0; i < JOBS; ++i) {
        if (children[i] <= 0) continue;
        int status;
        struct rusage usage;
        if (wait4(children[i], &status, 0, &usage) == -1) {
            if (errno != EINTR) perror("wait4");
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &ends[i]); // Record finish timestamp
        double user = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6; // User mode CPU time
        double system = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6; // Kernel mode CPU time
        double elapsed = (ends[i].tv_sec - starts[i].tv_sec) +
                         (ends[i].tv_nsec - starts[i].tv_nsec) / 1e9; // Wall-clock elapsed time
        printf("%s,%ld,%d,%.6f,%.6f,%.6f,%ld,%ld\n",
               jobs[i].name, (long)children[i], jobs[i].nice_value,
               user, system, elapsed, usage.ru_nvcsw, usage.ru_nivcsw);
        children[i] = 0;
    }

    // Reap any remaining or interrupted child processes
    for (int i = 0; i < JOBS; ++i) {
        if (children[i] > 0) {
            int status;
            while (waitpid(children[i], &status, 0) == -1 && errno == EINTR) {}
            children[i] = 0;
        }
    }
    return stopping ? 128 : 0;
}
