/* tools/pisim.c — run a program inside limits that roughly match a Raspberry Pi
 * (Windows development aid; uses a job object).
 *
 *   pisim [--model zero|zero2|3|4|5] [--cpu PCT] [--mem MB] -- program args...
 *
 * The process is pinned to ONE core, its CPU is hard-capped to PCT percent of
 * that core, and its memory is capped at MB. Every 10 s (and at exit) it
 * prints memory and CPU-time figures so you can see how much headroom is left.
 *
 * Model presets (one core of this PC ~ an i5-13600KF P-core; Pi figures from
 * single-core benchmark ratios — rough, deliberately a bit pessimistic):
 *   zero   Pi Zero / 1        ARM11 1 GHz        2.5 % of a core   512 MB
 *   zero2  Pi Zero 2 W        A53  1.0 GHz       4   %             512 MB
 *   3      Pi 3 (A+/B/B+)     A53  1.2-1.4 GHz   5   %             512 MB
 *   4      Pi 4               A72  1.5-1.8 GHz   14  %             1024 MB
 *   5      Pi 5               A76  2.4 GHz       35  %             2048 MB */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name, *label;
    double cpu;
    int mem;
} MODELS[] = {
    {"zero", "Raspberry Pi Zero / 1 (ARM11 1 GHz)", 2.5, 512},
    {"zero2", "Raspberry Pi Zero 2 W (Cortex-A53 1 GHz)", 4.0, 512},
    {"3", "Raspberry Pi 3 (Cortex-A53 1.2-1.4 GHz)", 5.0, 512},
    {"4", "Raspberry Pi 4 (Cortex-A72 1.5-1.8 GHz)", 14.0, 1024},
    {"5", "Raspberry Pi 5 (Cortex-A76 2.4 GHz)", 35.0, 2048},
};

static double seconds(LARGE_INTEGER t) { return (double)t.QuadPart / 1e7; } /* 100 ns units */

static void report(HANDLE job, DWORD started, const char *when) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc;
    memset(&li, 0, sizeof li);
    memset(&acc, 0, sizeof acc);
    QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li, NULL);
    QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc, sizeof acc, NULL);
    double cpu = seconds(acc.TotalUserTime) + seconds(acc.TotalKernelTime);
    double wall = (GetTickCount() - started) / 1000.0;
    fprintf(stderr, "[pisim] %-6s peak memory %.1f MB | cpu used %.2f s in %.0f s wall\n", when,
            li.PeakProcessMemoryUsed / 1048576.0, cpu, wall);
}

int main(int argc, char **argv) {
    double cpu = 5.0;
    int mem = 512;
    const char *label = "custom limits";
    int i = 1;
    for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            const char *m = argv[++i];
            int found = 0;
            for (size_t k = 0; k < sizeof MODELS / sizeof MODELS[0]; k++) {
                if (!strcmp(MODELS[k].name, m)) {
                    cpu = MODELS[k].cpu;
                    mem = MODELS[k].mem;
                    label = MODELS[k].label;
                    found = 1;
                }
            }
            if (!found) {
                fprintf(stderr, "unknown model %s (zero, zero2, 3, 4, 5)\n", m);
                return 2;
            }
        } else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
            cpu = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--mem") && i + 1 < argc) {
            mem = atoi(argv[++i]);
        } else {
            fprintf(stderr, "usage: pisim [--model zero|zero2|3|4|5] [--cpu PCT] [--mem MB] -- program args...\n");
            return 2;
        }
    }
    if (i + 1 >= argc) {
        fprintf(stderr, "usage: pisim [--model ...] -- program args...\n");
        return 2;
    }

    /* command line for the child: the arguments after "--", quoted */
    char cmd[8192] = "";
    size_t used = 0;
    for (int k = i + 1; k < argc; k++) {
        int n = snprintf(cmd + used, sizeof cmd - used, k > i + 1 ? " \"%s\"" : "\"%s\"", argv[k]);
        if (n < 0 || (size_t)n >= sizeof cmd - used) {
            fprintf(stderr, "command line too long\n");
            return 2;
        }
        used += (size_t)n;
    }

    SYSTEM_INFO sys;
    GetSystemInfo(&sys);
    HANDLE job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_AFFINITY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    li.BasicLimitInformation.Affinity = 1; /* one logical processor */
    li.ProcessMemoryLimit = (SIZE_T)mem << 20;
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate;
    memset(&rate, 0, sizeof rate);
    rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    /* CpuRate is in 1/100 % of the WHOLE machine: scale "percent of one core" down */
    DWORD r = (DWORD)(cpu * 100.0 / sys.dwNumberOfProcessors + 0.5);
    rate.CpuRate = r ? r : 1;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
        !SetInformationJobObject(job, JobObjectCpuRateControlInformation, &rate, sizeof rate)) {
        fprintf(stderr, "[pisim] cannot set up limits (error %lu)\n", GetLastError());
        return 1;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[pisim] cannot start %s (error %lu)\n", argv[i + 1], GetLastError());
        return 1;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        fprintf(stderr, "[pisim] cannot apply limits (error %lu)\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    fprintf(stderr, "[pisim] %s: 1 core capped at %.1f%% (job rate %lu/10000 of %lu CPUs), memory cap %d MB\n", label,
            cpu, (unsigned long)rate.CpuRate, (unsigned long)sys.dwNumberOfProcessors, mem);
    SetConsoleCtrlHandler(NULL, TRUE); /* Ctrl+C goes to the child; we wait for it */
    DWORD started = GetTickCount();
    ResumeThread(pi.hThread);
    while (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) report(job, started, "now");
    report(job, started, "exit");
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
