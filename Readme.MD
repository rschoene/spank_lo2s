# SPANK lo2s plugin

This project contains a Slurm SPANK plugin that starts and manages the `lo2s` profiler for a job. The plugin runs during the Slurm job lifecycle and forwards job metadata and cgroup information to `lo2s` so that output can be captured under a job-specific directory.

## What this plugin does

- registers SPANK options such as:
  - `--lo2s`
  - `--lo2s-event`
  - `--lo2s-event-rate`
  - `--lo2s-metrics`
  - `--lo2s-metrics-frequency`
  - `--lo2s-standard-metrics`
- starts a background daemon during the job prolog
- writes the runtime arguments to a FIFO in `/tmp`
- finds the Slurm job cgroup and moves the daemon into it
- starts `lo2s` with the configured output directory
- shuts the daemon down during the job epilog
- attempts to fix file ownership for generated output

## Project files

- `spank_lo2s.c` — the SPANK plugin implementation
- `Makefile` — build rules for the plugin
- `slurm/` — local Slurm headers used for compilation

## Build requirements

You need:

- a Slurm installation with SPANK headers available
- the `lo2s` binary installed on the compute nodes
- a compiler toolchain (`gcc` is used by default)
- permissions to install the resulting `.so` into the Slurm plugin path

## Basic build

From the project root:

```bash
make
```

If `lo2s` is not in the default location, build it explicitly:

```bash
make LO2S_BINARY_PATH=/opt/lo2s/bin/lo2s
```

The plugin also supports a debug build:

```bash
make debug
```

A debug build with GDB crash backtrace capture (defines `GDB`, so the plugin runs `lo2s` under GDB in batch mode and writes a backtrace to the log on crash):

```bash
make debug-gdb
```

A release build:

```bash
make release
```

## Install

After building, install the resulting shared object to the Slurm SPANK plugin directory used by your cluster, for example:

```bash
cp spank_lo2s.so /usr/lib64/slurm/spank/
```

On some systems the directory may be different, such as:

- `/usr/lib/slurm` 
- `/usr/local/lib/slurm`
- `/etc/slurm/plugstack.conf`-managed plugin directories

Adapt the destination to match your Slurm installation.

## Required system-specific adaptations

This code is not fully generic as-is. The following values are currently hard-coded and must be checked against your cluster environment:

### 1. `lo2s` binary path

In `spank_lo2s.c`:

```c
#ifndef LO2S_BINARY_PATH
#define LO2S_BINARY_PATH "/usr/bin/lo2s"
#endif
```

If your `lo2s` binary is installed somewhere else, change this value or pass it at build time:

```bash
make LO2S_BINARY_PATH=/path/to/lo2s
```

### 2. cgroup location

The plugin searches for the job cgroup under:

```c
#ifndef CGROUP_FOLDER
#define CGROUP_FOLDER "/sys/fs/cgroup"
#endif
```

This assumes a standard Linux cgroup mount. If your system uses a different cgroup root, systemd layout, or cgroup v2 semantics, update this value or adjust the search logic in `lo2d_daemon()` and `slurm_spank_init_post_opt()`.

### 3. temporary communication FIFOs

The plugin creates communication pipes such as:

```c
static char _lo2s_cfg_comm_path_template[2048] = "/tmp/spank_lo2s_%d";
static char _lo2s_cfg_pid_path_template[2048] = "/tmp/spank_lo2s_%d_pid";
```

These paths assume a shared writable `/tmp` on the node. If your system mounts `/tmp` differently, uses a local scratch filesystem, or has restrictions on shared job tmp space, adjust these templates.

### 4. output path for `lo2s`

The plugin writes output into a path constructed from the job ID and hostname, such as:

```c
snprintf(buffer, sizeof(buffer), "-A --cgroup %s -o %s/%d/%s --dwarf full", cgroup_path, _lo2s_trace_path, jid, hostname);
```

The user-defined output path is passed through the SPANK option `--lo2s`. This directory must:

- exist on the compute node
- be writable by the job user
- be readable by the Slurm runtime/user context that needs to access the results
- match the filesystem layout and policy of your cluster

### 5. Slurm installation layout

The code includes:

```c
#include <slurm/spank.h>
```

This requires the Slurm development headers to match the cluster version. If your Slurm version differs from the headers installed in this workspace, you must either:

- install matching development headers
- use the correct include path
- rebuild against the Slurm version used by the cluster

### 6. Slurm configuration and plugin loading

This plugin must be loaded through your Slurm configuration. In practice, you typically need to ensure:

- the `.so` file is installed in the correct Slurm plugin path
- the SPANK plugin is enabled via `plugstack.conf` or equivalent Slurm configuration
- the relevant prolog/epilog or job launch hooks are active in your cluster configuration

The exact steps depend on the Slurm distribution and the cluster policy.

## Typical plugin usage

The plugin expects the `lo2s` output path to be provided through the job launch command or environment. In practice, the cluster administrator can configure a Slurm `--lo2s` option or a wrapper script that passes the path to the job.

Example conceptually:

```bash
srun --lo2s=/scratch/trace_data my_job
```

The plugin then starts `lo2s` with the generated cgroup and output path when the job runs.

## Operational notes

- The plugin currently uses compile-time constants rather than a runtime config file.
- It makes assumptions about cgroup discovery and path layouts that may not match all systems.
- Debug logging is enabled only when `make debug` is used.
- If the `lo2s` binary is missing, the plugin logs and exits gracefully without failing the entire job.
- The code attempts to recursively `chown` the generated output tree to the job user after the job finishes.

## Recommended adaptation checklist

Before deploying this on a production system, verify:

1. the `lo2s` binary path is correct
2. the cgroup root matches the actual Slurm environment
3. the output directory is writable and accessible
4. `/tmp` is suitable for FIFO communication on all nodes
5. the Slurm plugin directory is correct for this installation
6. the plugin is loaded through the cluster’s Slurm configuration
7. the matching Slurm headers and runtime library are installed

## License

This repository does not currently declare a project license. If you intend to distribute or deploy it in a cluster environment, confirm the license and any site-specific constraints before redistribution.
