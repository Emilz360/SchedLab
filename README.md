# SchedLab source code

## Linux build
```bash
make
```

## Run the CPU scheduler simulator
```bash
./build/schedlab workloads/sample.txt 2 5
```
Arguments: workload path, Round Robin quantum, priority-aging interval.

Workload line format:
```text
NAME ARRIVAL PRIORITY CPU[,IO,CPU...]
```
CPU and I/O bursts alternate; the list must start and end with a CPU burst.

Algorithms:
- FCFS: non-preemptive.
- SJF: non-preemptive, selects the shortest next CPU burst.
- SRTF: preemptive, selects the smallest remaining CPU burst.
- Priority with Aging: preemptive; lower numeric priority is higher, and waiting improves effective priority.
- Round Robin: time-sliced using the supplied quantum.

## Linux real scheduler experiment
```bash
./build/linux_launcher 8 1 > linux_1core.csv
./build/linux_launcher 8 2 > linux_2cores.csv
./build/linux_launcher 8 4 > linux_4cores.csv
```
The launcher selects the first N CPUs permitted by the current process affinity mask. Run on a quiet machine and repeat each configuration several times.

## Windows build
Open a Visual Studio Developer Command Prompt:
```bat
cl /O2 src\windows_worker.c
cl /O2 src\windows_launcher.c
windows_launcher.exe 8 1
```
Keep `windows_worker.exe` in the same directory as `windows_launcher.exe`.

## Notes
- Windows GetProcessTimes provides user and kernel CPU time, not context-switch counters.
- Run experiments with bounded durations in a VM or test machine.
- The sample workload is for testing; replace it with the lecturer's TD/TP and Chapter 2 examples for validation.
- This code is a project starter. Verify the simulator output against hand calculations before submitting.
