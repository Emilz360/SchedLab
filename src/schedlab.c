#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define MAXP 64 // Maximum number of processes supported in a workload
#define MAXB 32 // Maximum number of CPU and I/O bursts per process
#define NAMELEN 32 // Maximum string length for process names
#define MAXLINE 512 // Buffer size for reading lines from workload file

// Process lifecycle states matching standard operating system models
typedef enum { NEW, READY, RUNNING, BLOCKED, FINISHED } State;

// Supported CPU scheduling algorithms
typedef enum { FCFS, SJF, SRTF, PRIORITY_AGING, ROUND_ROBIN } Algorithm;

// Process Control Block (PCB) storing process configuration and runtime metrics
typedef struct {
    char name[NAMELEN]; // Process name identifier
    int arrival, priority; // Arrival time and base priority (lower value = higher priority)
    int bursts[MAXB], burst_count, burst_index; // Alternating CPU/IO burst durations
    int remaining, first_start, finish, waiting; // Burst remaining time, first CPU tick, finish tick, and wait time
    int ready_since, io_until, quantum_used; // Ready queue entry time, I/O completion tick, and RR quantum used
    State state; // Current process state
} Process;

static Process proc[MAXP]; // Global process table
static int process_count = 0; // Total count of loaded processes
static int rr_quantum = 2; // Default Round Robin time quantum
static int aging_interval = 5; // Waiting ticks required to improve priority by 1

static const char *algorithm_name[] = {
    "FCFS", "SJF", "SRTF", "Priority with Aging", "Round Robin"
};

// Parses a workload file of processes with their arrival times, priorities, and alternating CPU/IO bursts into the global process table
static int load_workload(const char *filename) {
    FILE *fp = fopen(filename, "r");
    char line[MAXLINE];

    if (!fp) {
        perror("Cannot open workload");
        return 0;
    }

    while (fgets(line, sizeof line, fp)) {
        char *token;
        Process p;
        memset(&p, 0, sizeof p);

        // Skip blank lines and comment lines starting with '#'
        token = strtok(line, " \t\r\n");
        if (!token || token[0] == '#') continue;
        if (process_count >= MAXP) {
            fprintf(stderr, "Maximum process count (%d) exceeded.\n", MAXP);
            fclose(fp);
            return 0;
        }

        snprintf(p.name, sizeof p.name, "%s", token); // Store process name

        token = strtok(NULL, " \t\r\n");
        if (!token) goto invalid_line;
        p.arrival = atoi(token); // Parse arrival time

        token = strtok(NULL, " \t\r\n");
        if (!token) goto invalid_line;
        p.priority = atoi(token); // Parse base priority

        token = strtok(NULL, " \t\r\n");
        if (!token) goto invalid_line;

        // Parse comma-separated list of alternating CPU and I/O bursts
        char *burst = strtok(token, ",");
        while (burst) {
            int value = atoi(burst);
            if (value <= 0 || p.burst_count >= MAXB) {
                fprintf(stderr, "Invalid CPU/I/O burst for process %s.\n", p.name);
                fclose(fp);
                return 0;
            }
            p.bursts[p.burst_count++] = value;
            burst = strtok(NULL, ",");
        }

        // Validate that arrival is non-negative and burst count is odd (starts and ends with CPU)
        if (p.arrival < 0 || p.burst_count == 0 || p.burst_count % 2 == 0) {
            fprintf(stderr, "Invalid workload line for %s.\n", p.name);
            fclose(fp);
            return 0;
        }

        p.remaining = p.bursts[0]; // Set remaining duration for initial CPU burst
        p.first_start = -1; // -1 indicates process has not yet acquired CPU
        p.state = NEW; // Initial state is NEW
        proc[process_count++] = p;
        continue;

invalid_line:
        fprintf(stderr, "Malformed workload line: %s", line);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    return process_count > 0;
}

// Resets runtime state of all processes back to initial baseline before running an algorithm
static void reset_processes(void) {
    for (int i = 0; i < process_count; ++i) {
        proc[i].burst_index = 0;
        proc[i].remaining = proc[i].bursts[0];
        proc[i].first_start = -1;
        proc[i].finish = 0;
        proc[i].waiting = 0;
        proc[i].ready_since = proc[i].arrival;
        proc[i].io_until = 0;
        proc[i].quantum_used = 0;
        proc[i].state = NEW;
    }
}

// Returns 1 if all processes have completed execution, 0 otherwise
static int all_finished(void) {
    for (int i = 0; i < process_count; ++i)
        if (proc[i].state != FINISHED) return 0;
    return 1;
}

// Computes dynamic priority: priority improves (decreases) by 1 for each aging_interval ticks spent waiting
static int effective_priority(int i) {
    return proc[i].priority - proc[i].waiting / aging_interval;
}

// Returns 1 if candidate process is preferred over current best according to active algorithm rules
static int better_candidate(Algorithm alg, int candidate, int current) {
    if (current < 0) return 1; // If no process is currently chosen, candidate wins by default

    // FCFS and Round Robin select the process that entered READY state earliest
    if (alg == FCFS || alg == ROUND_ROBIN) {
        if (proc[candidate].ready_since != proc[current].ready_since)
            return proc[candidate].ready_since < proc[current].ready_since;
        return candidate < current; // Tie-breaker by process table index
    }

    // SJF and SRTF select the process with the shortest remaining CPU burst
    if (alg == SJF) {
        if (proc[candidate].remaining != proc[current].remaining)
            return proc[candidate].remaining < proc[current].remaining;
    } else if (alg == SRTF) {
        if (proc[candidate].remaining != proc[current].remaining)
            return proc[candidate].remaining < proc[current].remaining;
    } else if (alg == PRIORITY_AGING) { // Priority with Aging selects lowest numeric effective priority
        int a = effective_priority(candidate);
        int b = effective_priority(current);
        if (a != b) return a < b;
    }

    // Tie-breaker: earlier ready time, then lower table index
    if (proc[candidate].ready_since != proc[current].ready_since)
        return proc[candidate].ready_since < proc[current].ready_since;
    return candidate < current;
}

// Selects the next process to execute on CPU for current time unit (-1 if idle)
static int choose_process(Algorithm alg, int running, int time) {
    // Non-preemptive algorithms (FCFS, SJF) retain running process until its burst completes
    if ((alg == FCFS || alg == SJF) &&
        running >= 0 && proc[running].state == RUNNING)
        return running;

    int best = -1;
    // Search ready queue for the best candidate that has already arrived
    for (int i = 0; i < process_count; ++i) {
        if (proc[i].state != READY || proc[i].arrival > time) continue;
        if (better_candidate(alg, i, best)) best = i;
    }

    // Preemptive algorithms (SRTF, Priority) only preempt running process if candidate is strictly better
    if ((alg == SRTF || alg == PRIORITY_AGING) &&
        running >= 0 && proc[running].state == RUNNING) {
        if (best < 0) return running;
        if (!better_candidate(alg, best, running)) return running;
    }
    return best;
}

// Simulates a scheduling algorithm tick by tick, printing Gantt chart and performance metrics
static void run_algorithm(Algorithm alg) {
    reset_processes(); // Reset processes to identical initial state

    int time = 0, running = -1, previous_cpu = -1;
    int busy = 0, context_switches = 0, completed = 0;
    (void)completed; // Suppress unused warning
    int *timeline = calloc(1000000, sizeof(int)); // Timeline storing running process ID per tick
    if (!timeline) {
        perror("calloc");
        return;
    }

    while (!all_finished() && time < 999999) {
        // Admit newly arrived processes and wake processes whose I/O bursts finished
        for (int i = 0; i < process_count; ++i) {
            if (proc[i].state == NEW && proc[i].arrival <= time) {
                proc[i].state = READY;
                proc[i].ready_since = time;
            } else if (proc[i].state == BLOCKED && proc[i].io_until <= time) {
                proc[i].burst_index++; // Advance to next CPU burst
                proc[i].remaining = proc[i].bursts[proc[i].burst_index];
                proc[i].state = READY;
                proc[i].ready_since = time;
            }
        }

        // Round Robin quantum expiry: move running process to tail of ready queue
        if (alg == ROUND_ROBIN && running >= 0 &&
            proc[running].state == RUNNING &&
            proc[running].quantum_used >= rr_quantum) {
            proc[running].state = READY;
            proc[running].ready_since = time;
            proc[running].quantum_used = 0;
            running = -1;
        }

        int selected = choose_process(alg, running, time); // Choose process for current tick

        // Handle context switch if running process changes
        if (selected != running) {
            if (selected >= 0 && previous_cpu >= 0 && selected != previous_cpu)
                context_switches++;
            if (running >= 0 && proc[running].state == RUNNING) {
                proc[running].state = READY;
                proc[running].ready_since = time;
            }
            running = selected;
            if (running >= 0) {
                proc[running].state = RUNNING;
                proc[running].quantum_used = 0;
            }
        }

        timeline[time] = running; // Record which process occupied CPU at this time tick

        // Accumulate waiting time for every process waiting in READY state
        for (int i = 0; i < process_count; ++i)
            if (proc[i].state == READY) proc[i].waiting++;

        // Execute one time unit of work on the CPU
        if (running >= 0) {
            Process *p = &proc[running];
            if (p->first_start < 0) p->first_start = time; // Record initial response timestamp
            p->remaining--; // Decrement remaining burst time
            p->quantum_used++; // Increment quantum counter
            busy++; // Increment total CPU busy time
            previous_cpu = running;

            // Handle completion of current CPU burst
            if (p->remaining == 0) {
                p->burst_index++;
                if (p->burst_index >= p->burst_count) {
                    p->state = FINISHED; // Process finished all bursts
                    p->finish = time + 1;
                    completed++;
                } else {
                    int io_duration = p->bursts[p->burst_index];
                    p->state = BLOCKED; // Block process for duration of I/O burst
                    p->io_until = time + 1 + io_duration;
                }
                running = -1; // CPU becomes idle until next dispatch
            }
        }
        time++;
    }

    if (!all_finished()) {
        fprintf(stderr, "Simulation stopped at safety limit.\n");
        free(timeline);
        return;
    }

    // Print ASCII Gantt Chart
    printf("\n==================================================\n");
    printf("ALGORITHM: %s\n", algorithm_name[alg]);
    printf("GANTT CHART (one character cell per time unit):\n");
    for (int t = 0; t < time; ++t) {
        if (timeline[t] < 0) printf("| IDLE ");
        else printf("| %-5s", proc[timeline[t]].name);
    }
    printf("|\n");
    printf("Time: ");
    for (int t = 0; t <= time; ++t) printf("%-6d", t);
    printf("\n\n");

    // Calculate and display process performance statistics
    double total_wait = 0, total_turnaround = 0, total_response = 0;
    printf("%-12s %10s %12s %10s %10s\n",
           "Process", "Waiting", "Turnaround", "Response", "Finish");

    for (int i = 0; i < process_count; ++i) {
        int turnaround = proc[i].finish - proc[i].arrival; // Turnaround = Finish - Arrival
        int response = proc[i].first_start - proc[i].arrival; // Response = First Run - Arrival
        printf("%-12s %10d %12d %10d %10d\n",
               proc[i].name, proc[i].waiting, turnaround,
               response, proc[i].finish);
        total_wait += proc[i].waiting;
        total_turnaround += turnaround;
        total_response += response;
    }

    // Display summary averages and CPU utilisation
    printf("\nAverage waiting time:    %.2f\n", total_wait / process_count);
    printf("Average turnaround time: %.2f\n", total_turnaround / process_count);
    printf("Average response time:   %.2f\n", total_response / process_count);
    printf("CPU utilisation:         %.2f%%\n", 100.0 * busy / time);
    printf("Context switches:        %d\n", context_switches);
    free(timeline);
}

// Parses command-line arguments, loads workload, and simulates all scheduling algorithms
int main(int argc, char **argv) {
    // Default workload so the program also works when launched from an IDE with no arguments
    const char *path = argv[1];
    if (argc < 2) {
        const char *candidates[] = {"workloads/sample.txt", "../workloads/sample.txt",
                                    "/home/sakwe/Desktop/SchedLab/workloads/sample.txt"};
        path = NULL;
        for (int i = 0; i < 3; ++i) {
            FILE *f = fopen(candidates[i], "r");
            if (f) { fclose(f); path = candidates[i]; break; }
        }
        if (!path) {
            fprintf(stderr, "Usage: %s workload.txt [RR_quantum] [aging_interval]\n", argv[0]);
            return 1;
        }
        printf("No workload given, using %s\n\n", path);
    }
    if (argc >= 3) rr_quantum = atoi(argv[2]);
    if (argc >= 4) aging_interval = atoi(argv[3]);
    if (rr_quantum < 1 || aging_interval < 1) {
        fprintf(stderr, "Quantum and aging interval must be positive.\n");
        return 1;
    }
    if (!load_workload(path)) return 1;

    // Run each scheduling algorithm sequentially on the loaded workload
    for (int a = FCFS; a <= ROUND_ROBIN; ++a)
        run_algorithm((Algorithm)a);
    return 0;
}
