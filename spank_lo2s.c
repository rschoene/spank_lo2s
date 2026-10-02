#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <slurm/spank.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>

// RLIMIT
#ifdef GDB
 #include <sys/resource.h>
#endif


// Name and version of the plugin
SPANK_PLUGIN(lo2s, 1);

// TODO: read these from config /etc/slurm/lo2s.conf
#ifndef LO2S_BINARY_PATH
static char _lo2s_cfg_path[] = "/usr/bin/lo2s";
#else
  #define STRINGIFY(x) #x
  #define TOSTRING(x) STRINGIFY(x)
  static char _lo2s_cfg_path[] = TOSTRING(LO2S_BINARY_PATH);
#endif

// fix this for if LO2S_BINARY_PATH is defined externally. Then it is not a string literal and we cannot use it in a static char array. We will have to use a macro to convert it to a string literal.


#ifndef CGROUP_FOLDER
#define CGROUP_FOLDER "/sys/fs/cgroup"
#endif
static char _lo2s_cfg_cgroup_folder[2048] = CGROUP_FOLDER;


static char _lo2s_cfg_comm_path_template[2048] = "/tmp/spank_lo2s_%d";
static char _lo2s_cfg_pid_path_template[2048] = "/tmp/spank_lo2s_%d_pid";
static char _lo2s_cfg_output_path_template[2048] = "/tmp/spank_lo2s_%d_path";

#ifndef LO2S_SHUTDOWN_TIMEOUT_MS
#define LO2S_SHUTDOWN_TIMEOUT_MS 60000
#endif
static int _lo2s_cfg_shutdown_timeout_ms = LO2S_SHUTDOWN_TIMEOUT_MS;

#ifdef LO2S_DEBUG

#ifndef LO2D_DEBUG_PATH
#define LO2D_DEBUG_PATH "/tmp/slurm_spank_lo2s_daemon_%d.%d-%d.log"
#pragma message "LO2D_DEBUG_PATH is not defined, using default: " LO2D_DEBUG_PATH
#endif

#ifndef LO2S_PROLOG_DEBUG_PATH
#define LO2S_PROLOG_DEBUG_PATH "/tmp/slurm_spank_lo2s_prolog_%d.%d-%d.log"
#pragma message "LO2S_PROLOG_DEBUG_PATH is not defined, using default: " LO2S_PROLOG_DEBUG_PATH
#endif

#ifndef LO2S_INIT_DEBUG_PATH
#define LO2S_INIT_DEBUG_PATH "/tmp/slurm_spank_lo2s_init_%d.%d-%d.log"
#pragma message "LO2S_INIT_DEBUG_PATH is not defined, using default: " LO2S_INIT_DEBUG_PATH
#endif

#ifndef LO2S_EPILOG_DEBUG_PATH
#define LO2S_EPILOG_DEBUG_PATH "/tmp/slurm_spank_lo2s_epilog_%d.%d-%d.log"
#pragma message "LO2S_EPILOG_DEBUG_PATH is not defined, using default: " LO2S_EPILOG_DEBUG_PATH
#endif

#ifndef LO2S_INIT_POST_OPT_DEBUG_PATH
#define LO2S_INIT_POST_OPT_DEBUG_PATH "/tmp/slurm_spank_lo2s_init_post_opt_%d.%d-%d.log"
#pragma message "LO2S_INIT_POST_OPT_DEBUG_PATH is not defined, using default: " LO2S_INIT_POST_OPT_DEBUG_PATH
#endif

#define OPEN_LOG(PATH, JOB, STEP, CONTEXT, RET_ERROR) \
    FILE *log_file = NULL; \
    char buffer_rnsorgvnproopn[1024]; \
    snprintf(buffer_rnsorgvnproopn, sizeof(buffer_rnsorgvnproopn), PATH, JOB, STEP, CONTEXT); \
    log_file = fopen(buffer_rnsorgvnproopn, "w"); \
    if (!log_file) { \
        return RET_ERROR; \
    }
#define LOG(fmt, ...) \
    if (log_file) { \
        fprintf(log_file, fmt, ##__VA_ARGS__); \
        fflush(log_file); \
    }
#define CLOSE_LOG() \
    if (log_file) { \
        fclose(log_file); \
        log_file = NULL; \
    }
#else

// mark log_file as unused to avoid compiler warnings
#define OPEN_LOG(PATH, JOB, STEP, CONTEXT, RET_ERROR) \
    (void)JOB; \
    (void)STEP; \
    (void)CONTEXT; \
    FILE *log_file = NULL; \
    (void)log_file;

#define LOG(fmt, ...) {}
#define CLOSE_LOG() {}

#endif

typedef enum {
    SPANK_STATE_EMPTY = 0, // before prolog and after epilog, no pipe file, no pid file, no path file
    SPANK_STATE_DAEMON_WAITS = 1, // after prolog, before init_post_opt, pipe file exists, pid file exists, path file does not exist
    SPANK_STATE_DAEMON_RUNNING = 2, // after init_post_opt, pipe file does not exist, pid file exists, path file exists
    SPANK_STATE_DAEMON_DEAD = 3, // after init_post_opt, pid file exists, path file exists, but pid is not alive
    SPANK_STATE_DAEMON_NOT_ALIVE_PIPE_EXISTS = 4, // can be before the daemon starts, after prolog until 
    SPANK_STATE_UNKNOWN = 5
} spank_lo2s_state_t;

static spank_lo2s_state_t _get_spank_lo2s_state(int job_id, FILE* log_file) {
    bool pipe_exists = false, pid_exists = false, path_exists = false, pid_alive = false;
    // check if the pipe file exists
    char path[1024];
    snprintf(path, sizeof(path), _lo2s_cfg_comm_path_template, job_id);
    pipe_exists = (access(path, F_OK) == 0);
    snprintf(path, sizeof(path), _lo2s_cfg_pid_path_template, job_id);
    pid_exists = (access(path, F_OK) == 0);
    if (pid_exists) {
        // check if the pid is alive
        FILE *pid_file = fopen(path, "r");
        if (pid_file) {
            int pid;
            fscanf(pid_file, "%d", &pid);
            fclose(pid_file);
            if (kill(pid, 0) == 0) {
                pid_alive = true;
            }
        }
    }
    snprintf(path, sizeof(path), _lo2s_cfg_output_path_template, job_id);
    path_exists = (access(path, F_OK) == 0);
    // if there is neither a pipe file, nor a path file, nor a pid file, then the state is SPANK_STATE_EMPTY
    if (!pipe_exists && !pid_exists && !path_exists) {
        return SPANK_STATE_EMPTY;
    }
    // if there is a pipe file, but no pid file, then the state is SPANK_STATE_DAEMON_NOT_ALIVE_PIPE_EXISTS
    if (pipe_exists && !pid_exists) {
        return SPANK_STATE_DAEMON_NOT_ALIVE_PIPE_EXISTS;
    }
    // if there is a pipe file and a pid file, but the pid is not alive, then the state is SPANK_STATE_DAEMON_DEAD
    if (pid_exists && !pid_alive) {
        return SPANK_STATE_DAEMON_DEAD;
    }
    // the pipe is deleted before lo2s starts
    if (pipe_exists && pid_exists && pid_alive) {
        return SPANK_STATE_DAEMON_WAITS;
    }
    // if there is a path file, then the state is SPANK_STATE_DAEMON_RUNNING
    if (!pipe_exists && path_exists && pid_exists && pid_alive) {
        return SPANK_STATE_DAEMON_RUNNING;
    }
    return SPANK_STATE_UNKNOWN;
}

// Open a FIFO for writing without blocking. A blocking open() of a FIFO for
// writing waits until a reader opens the other end, which would hang the job's
// init path forever if the lo2s daemon already exited (timeout/crash) or was
// never started (fork failure). Opening with O_WRONLY|O_NONBLOCK returns
// immediately with ENXIO when no reader is present, so we can detect that and
// skip instead of hanging.
static FILE *open_fifo_for_write(const char *path) {
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return NULL; // no reader (ENXIO) or other error
    }
    // A reader is present; drop O_NONBLOCK so subsequent writes block normally
    // if the pipe buffer is momentarily full (safe: a reader is guaranteed).
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
    }
    return f;
}


static int check_access(uid_t uid, gid_t gid, const char *path, FILE* log_file);
static int recursive_chown(uid_t uid, gid_t gid, char* path, FILE* log_file, int recursive);


// callback executed when the lo2s option is set

static char _lo2s_trace_path[2048] = "";
static int _lo2s_path_cb(int val, const char *optarg, int remote) {
    if (snprintf(_lo2s_trace_path, sizeof(_lo2s_trace_path), "%s", optarg) < 0) {
        return -1;
    }
    return 0;
}

// callback executed when the lo2s-event option is set
static char _lo2s_event[2048] = "cpu-cycles";
static int _lo2s_event_cb(int val, const char *optarg, int remote) {
    if (snprintf(_lo2s_event, sizeof(_lo2s_event), "%s", optarg) < 0) {
        return -1;
    }
    return 0;
}

// callback executed when the lo2s-event option is set
static int _lo2s_event_rate = 10;
static int _lo2s_event_rate_cb(int val, const char *optarg, int remote) {
    _lo2s_event_rate = atoi(optarg);
    return 0;
}

// callback executed when the lo2s-event option is set
static char _lo2s_metrics[512] = "";
static int _lo2s_metrics_cb(int val, const char *optarg, int remote) {
    if (snprintf(_lo2s_metrics, sizeof(_lo2s_metrics), "%s", optarg) < 0) {
        return -1;
    }
    return 0;
}

// callback executed when the lo2s-event option is set
static int _lo2s_metrics_frequency = 0;
static int _lo2s_metrics_frequency_cb(int val, const char *optarg, int remote) {
    _lo2s_metrics_frequency = atoi(optarg);
    return 0;
}

// callback executed when the lo2s-event option is set
static int _lo2s_standard_metrics = 0;
static int _lo2s_standard_metrics_cb(int val, const char *optarg, int remote) {
    _lo2s_standard_metrics = 1;
    return 0;
}

struct spank_option all_spank_options[] = {
    {
        // sets the arguments for the lo2s daemon, which will be passed to the lo2s process
        "lo2s", 
        "PATH",
        "pass the path for the output of the lo2s monitoring process, the path has to exist and be writable by the user. If not given, no monitoring.",
        1, 0, _lo2s_path_cb
    },
    {
        // set the event of lo2s
        "lo2s-event",
        "EVENT",
        "set the event for the lo2s monitoring process, use lo2s --list-events to see available events",
        1, 0, _lo2s_event_cb
    },
    {
        // set the event rate of lo2s
        "lo2s-event-rate",
        "RATE",
        "set the event rate for the lo2s monitoring process, use lo2s --list-events to see available events",
        1, 0, _lo2s_event_rate_cb
    },
    {
        // set lo2s metrics
        "lo2s-metrics",
        "METRICS",
        "set the metrics for the lo2s monitoring process, use lo2s --list-metrics to see available metrics",
        1, 0, _lo2s_metrics_cb
    },
    {
        // set lo2s metrics
        "lo2s-metrics-frequency",
        "FREQUENCY_IN_HZ",
        "set the frequency of the metrics for the lo2s monitoring process, use lo2s --list-metrics to see available metrics",
        1, 0, _lo2s_metrics_frequency_cb
    },
    {
        // set lo2s metrics
        "lo2s-standard-metrics",
        "FREQUENCY_IN_HZ",
        "set the frequency of the metrics for the lo2s monitoring process, use lo2s --list-metrics to see available metrics",
        0, 0, _lo2s_standard_metrics_cb
    },
    SPANK_OPTIONS_TABLE_END
};

static int lo2d_daemon(int job_id, char pipe_path_read[1024]) {

    OPEN_LOG(LO2D_DEBUG_PATH, job_id, 0, 0, -1);
    // tokenize the arguments and store them in an array
    char buffer[1024];
    // read exactly one line from the pipe file, which should contain the arguments for lo2s
    // initially there might not be anyone who writes to the file, so we will have to wait for an event.
    ssize_t total_bytes_read=0;
    int max_iterations=1000; // wait for 10 seconds max
    int iterations=0;
    ssize_t bytes_read=0;

    int pipe_fd=open(pipe_path_read, O_RDONLY);
    // read from the pipe file, which should contain the arguments for lo2s until we read "\n"
    do {
        bytes_read = read(pipe_fd, &buffer[total_bytes_read], sizeof(buffer) - 1-total_bytes_read);
        LOG("lo2d_daemon: Read bytes: %zd\n", bytes_read);
        if (bytes_read <= 0) {
                struct timespec ts;
                ts.tv_sec = 0;           // Seconds
                ts.tv_nsec = 100000000;  // Nanoseconds (100ms)

                int x=0;
                // Sleep for the specified time
                while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
                    // If interrupted by a signal, continue sleeping for the remaining time
                    if ( (x++)%200 == 0)
                        LOG("Sleep interrupted\n");
                }
        }
        iterations++;
        if (iterations >= max_iterations) {
            LOG("lo2d_daemon: Timeout while reading from pipe file\n");
            char pid_file_cleanup[1024];
            snprintf(pid_file_cleanup, sizeof(pid_file_cleanup), _lo2s_cfg_pid_path_template, job_id);
            unlink(pid_file_cleanup);
            LOG("lo2d_daemon: Cleaning up PID file\n");
            CLOSE_LOG();
            exit(1);
        }
        if (bytes_read > 0) {
            total_bytes_read += bytes_read;
        }
        LOG("lo2d_daemon: Total bytes read: %zd, current: %s\n", total_bytes_read, buffer);
    } while ((buffer[total_bytes_read - 1] != '\n' || buffer[total_bytes_read - 1] == '\0') || total_bytes_read == 0 );

    LOG("lo2d_daemon: Finished reading from pipe file, total bytes read: %zd\n", total_bytes_read);

    if (total_bytes_read <= 0) {
        LOG("lo2d_daemon: Failed to read from pipe file\n");
        CLOSE_LOG();
        exit(1);
    }
    strchr(buffer, '\n')[0] = '\0'; // replace newline with null terminator
 
    buffer[total_bytes_read] = '\0';

    if (strcmp(buffer, "CANCEL") == 0) {
        LOG("lo2d_daemon: CANCELLING lo2s\n");
        // Clean up the PID file
        LOG("lo2d_daemon: Cleaning up PID file\n");
        char pid_file_cleanup[1024];
        snprintf(pid_file_cleanup, sizeof(pid_file_cleanup), _lo2s_cfg_pid_path_template, job_id);
        unlink(pid_file_cleanup);
        CLOSE_LOG();
        exit(0);
    }

    // we do not need the pipe any longer
    close(pipe_fd);

    // unlink the pipe file, so that the lo2s daemon will exit when the job ends
    if (unlink(pipe_path_read) != 0) {
        LOG("lo2d_daemon: failed to unlink pipe file %s\n", pipe_path_read);
        CLOSE_LOG();
        exit(1);
    }
    
    char *argv[1024];
    int argc = 0;
    // if GDB is defined, we will use GDB to write the reason for a crash to the log file, so that we can debug it later. We will use the --args option to pass the arguments to lo2s, so that GDB can start lo2s with the correct arguments.
    // GDB should always return 0, so that the job does not fail if GDB is not installed. We will use the --batch option to run GDB in batch mode, so that it does not wait for user input. We will use the --ex option to run a command after starting GDB, which will be "run" to start lo2s. We will use the --ex option again to run a command after lo2s exits, which will be "bt" to print the backtrace of the crash. We will use the --ex option again to run a command after printing the backtrace, which will be "quit" to exit GDB.

    argv[argc++] = _lo2s_cfg_path;
    char *token = strtok(buffer, " ");
    while (token != NULL) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    LOG("lo2d_daemon: Finished tokenizing arguments\n");

    LOG("lo2d_daemon: Starting lo2s daemon with arguments:\n");
    for (int i = 0; i < argc; i++) {
        LOG("lo2d_daemon: argv[%d] = %s\n", i, argv[i]);
    }

    // now we try to find the cgroup of the job and move the daemon into that cgroup, so that it is killed when the job ends
    LOG("lo2d_daemon: Trying to find cgroup for job %d\n", job_id);

    char cgroup_path[3072] = {0};
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "/usr/bin/find %s -name 'job_%d' | head -n 1", _lo2s_cfg_cgroup_folder, job_id);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(cgroup_path, sizeof(cgroup_path), fp) != NULL) {
            cgroup_path[strcspn(cgroup_path, "\n")] = 0;
            LOG("lo2d_daemon: Found cgroup path: %s\n", cgroup_path);
        } else {
            LOG("lo2d_daemon: Failed to find cgroup for job %d\n", job_id);
            CLOSE_LOG();
            exit(1);
        }
        pclose(fp);
    } else {
        LOG("lo2d_daemon: Failed to execute command to find cgroup for job %d\n", job_id);
        CLOSE_LOG();
        return -1;
    }

    // skip the cgroup for now
    // create a directory in the cgroup path for the lo2s daemon, so that it is killed when the job ends
    char lo2s_cgroup_path[3072];
    snprintf(lo2s_cgroup_path, sizeof(lo2s_cgroup_path), "%s/lo2s_daemon", cgroup_path);
    if (mkdir(lo2s_cgroup_path, 0755) != 0) {
        LOG("lo2d_daemon: Failed to create cgroup directory %s\n", lo2s_cgroup_path);
        CLOSE_LOG();
        return -1;
    }
    LOG("lo2d_daemon: Created cgroup directory %s\n", lo2s_cgroup_path);

    // move the daemon into the cgroup by writing its PID to the cgroup.procs file
    char cgroup_procs_path[3072];
    snprintf(cgroup_procs_path, sizeof(cgroup_procs_path), "%s/cgroup.procs", lo2s_cgroup_path);
    FILE *cgroup_procs_file = fopen(cgroup_procs_path, "w");
    if (!cgroup_procs_file) {
        LOG("lo2d_daemon: Failed to open cgroup.procs file %s\n", cgroup_procs_path);
        CLOSE_LOG();
        return -1;
    }
    fprintf(cgroup_procs_file, "%d\n", getpid());
    fclose(cgroup_procs_file);
    LOG("lo2d_daemon: Moved daemon into cgroup %s\n", lo2s_cgroup_path);


#ifdef GDB
    // set stdout and err to /tmp/lo2s_daemon_<jobid>.log
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "/tmp/lo2s_%d.log", job_id);
    FILE *lo2s_out = fopen(log_path, "w");
    if (!lo2s_out) {
        LOG("lo2d_daemon: Failed to open log file %s\n", log_path);
        CLOSE_LOG();
        return -1;
    }
    dup2(fileno(lo2s_out), STDOUT_FILENO);
    dup2(fileno(lo2s_out), STDERR_FILENO);
    // print every argument to stderr, so that we can see what is passed to lo2s
    for (int i = 0; i < argc; i++) {
        fprintf(stderr, "lo2d_daemon: argv[%d] = %s\n", i, argv[i]);
    }
    LOG("lo2d_daemon: Redirected stdout and stderr to %s\n", log_path);
    // core dump in /tmp, so that we can debug it later
    chdir("/tmp");
    // no gdb needed, set RLIMIT_CORE to unlimited, so that we can get a core dump if lo2s crashes
    struct rlimit core_limit;
    core_limit.rlim_cur = RLIM_INFINITY;
    core_limit.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_CORE, &core_limit) != 0) {
        LOG("lo2d_daemon: Failed to set RLIMIT_CORE to unlimited\n");
        CLOSE_LOG();
        return -1;
    }
#endif // ifdef GDB
    int result = execv(_lo2s_cfg_path, argv);
    (void) result; // surpress warning, if logging is disabled
    // we will not get here, if execve fails
    LOG("lo2d_daemon: Failed to start lo2s daemon: %d\n", result);
    CLOSE_LOG();
    return 0;
}

int slurm_spank_init(spank_t sp, int ac, char **av) {
    struct spank_option* o = &all_spank_options[0];
    while (o->name) {
        spank_option_register(sp, o);
        o++;
    }
    return ESPANK_SUCCESS;
}



int slurm_spank_job_prolog(spank_t sp, int ac, char **av) {
    char hostname[1024];
    gethostname(hostname,sizeof(hostname));
    
    int jid, uid, gid, stepid;
    spank_get_item(sp, S_JOB_UID, &uid);
    spank_get_item(sp, S_JOB_GID, &gid);
    spank_get_item(sp, S_JOB_ID, &jid);
    spank_get_item(sp, S_JOB_STEPID, &stepid);

    int context = spank_context();

    OPEN_LOG(LO2S_PROLOG_DEBUG_PATH, jid, stepid, 0, ESPANK_SUCCESS);
    
    LOG( "SPANK init: job %d.%d (user: %d, group: %d) in context %d, pid/ppid=%d/%d\n", jid, stepid, uid, gid, context, getpid(), getppid());
    LOG("SPANK init: lo2s path: %s\n", _lo2s_trace_path);

    // check if _lo2s_path exists, if not log and return
    if (access(_lo2s_cfg_path, F_OK) != 0) {
        LOG("SPANK plugin lo2s: lo2s binary %s does not exist, skipping\n", _lo2s_cfg_path);
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing if lo2s binary does not exist
    }

    if (context == S_CTX_LOCAL) {
        LOG("SPANK init: In local context, skipping pid/ppid: %d/%d\n", getpid(), getppid());
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing in local context
    }
    if (context != S_CTX_JOB_SCRIPT) {
        LOG("SPANK init: Not in remote context, but in context %d, skipping\n", context);
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing in remote context
    }
    slurm_error("slurm_spank_prolog: Start");

    LOG("SPANK job prolog: Starting lo2s daemon for job %d.%d (user: %d, group: %d), pid/ppid: %d/%d\n", jid, stepid, uid, gid, getpid(), getppid());

    // if it does not exist, create a file in /tmp/spank_lo2s_<jobid> and /tmp/spank_lo2s_<jobid>_ret for later pipe communication with the lo2s daemon
    // the lo2s daemon will read the arguments from the file /tmp/spank_lo2s_<jobid> and write the return value to /tmp/spank_lo2s_<jobid>_ret
    char pipe_path_read[1024];
    snprintf(pipe_path_read, sizeof(pipe_path_read), _lo2s_cfg_comm_path_template, jid);
    // list everything under /tmp/ to log_file
    if (access(pipe_path_read, F_OK) != 0) {
        // this will be a FIFO pipe, so we will use mkfifo to create it
        int fifo_ok = mkfifo(pipe_path_read, 0666);
        if (fifo_ok == -1) {
            LOG("SPANK job prolog: failed to create pipe file %s\n", pipe_path_read);
            CLOSE_LOG();
            return ESPANK_SUCCESS;
        }
    } else {
        LOG( "SPANK job prolog: pipe file %s already exists\n", pipe_path_read);
        return ESPANK_SUCCESS;
    }
    LOG( "SPANK job prolog: created pipe file %s\n", pipe_path_read);
    CLOSE_LOG();
    // now that we have the pipe file, we can start the lo2s daemon in the background and pass the pipe file path to it
    int intermediate_child = fork();
    if (intermediate_child == 0) {
        // Redirect stdin/stdout/stderr to /dev/null otherwise this will hang :(
        int devnull = open("/dev/null", O_RDWR);
        if (devnull != -1) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        int max_fd = sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < max_fd; fd++)
            close(fd);

        // we will use setsid() to create a new session and detach from the controlling terminal
        setsid();
        int grandchild_pid = fork();
        if (grandchild_pid < 0) {
            exit(1);
        } else if (grandchild_pid > 0) {

            // Write our PID to a well-known file IMMEDIATELY, before blocking on the FIFO.
            // This allows the epilog to find and kill us even if we never receive a message.
            char pid_file[1024];
            snprintf(pid_file, sizeof(pid_file), _lo2s_cfg_pid_path_template, jid);
            FILE *pf = fopen(pid_file, "w");
            if (pf) {
                fprintf(pf, "%d\n", grandchild_pid);
                fclose(pf);
            }
            // intermediate child: exit, so that the grandchild is adopted by init and becomes a daemon
            exit(0);
        }
        // child process: start lo2s waiter
        //execl("/usr/bin/sleep", "sleep", "5", NULL);
        lo2d_daemon(jid, pipe_path_read);
        exit(0);
    } else if (intermediate_child > 0) {
        int status;
        // wait until child has created the pid file
        waitpid(intermediate_child, &status, 0); 
        // parent process: continue
//        LOG("SPANK job prolog: started lo2s daemon with PID %d\n", daemon_pid);
    } else {
//        LOG("SPANK job prolog: failed to fork lo2s daemon\n");
    }
//    LOG( "prolog success\n");

    slurm_error("slurm_spank_prolog: Done");
    return ESPANK_SUCCESS;
}

int slurm_spank_init_post_opt(spank_t sp, int ac, char **av) {


    int uid, gid, jid, stepid;
    spank_get_item(sp, S_JOB_UID, &uid);
    spank_get_item(sp, S_JOB_GID, &gid);
    spank_get_item(sp, S_JOB_ID, &jid);
    spank_get_item(sp, S_JOB_STEPID, &stepid);
    int context = spank_context();

    OPEN_LOG(LO2S_INIT_POST_OPT_DEBUG_PATH, jid, stepid, context, ESPANK_SUCCESS);

    // Check the SPANK context
    if (context != S_CTX_REMOTE) {
        LOG("SPANK init post opt: Not in remote context, but in context %d, skipping\n", context);
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing in remote context
    }

    if (stepid != 0) {
        LOG("SPANK init post opt: Step ID %d, skipping\n", stepid);
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing in step context
    }

    // check if _lo2s_path exists, if not log and return
    if (access(_lo2s_cfg_path, F_OK) != 0) {
        LOG("SPANK plugin lo2s: lo2s binary %s does not exist, skipping\n", _lo2s_cfg_path);
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing if lo2s binary does not exist
    }

    // open the pipe to the daemon and write the lo2s options to it
    char pipe_path_write[1024];
    snprintf(pipe_path_write, sizeof(pipe_path_write), _lo2s_cfg_comm_path_template, jid);
    // if file does not exist, log and return
    if (access(pipe_path_write, F_OK) != 0) {
        LOG("SPANK plugin lo2s: Pipe file %s does not exist, skipping\n", pipe_path_write);
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing if pipe file does not exist
    }


    // check if uid is the uid of this process, if not, we are in a different context and should not start the daemon
/*    if (uid != getuid()) {
        LOG("SPANK plugin lo2s: User of job %d is not the owner of the current process %d", uid, getuid());
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing in other contexts
    }*/
    slurm_error("slurm_spank_init_post_opt: In remote context, starting lo2s daemon for job %d.%d (user: %d, group: %d), pid/ppid: %d/%d\n", jid, stepid, uid, gid, getpid(), getppid());

    
    LOG( "SPANK init_post_opt: lo2s path: %s\n", _lo2s_trace_path);
    LOG( "SPANK plugin lo2s: Is User %d allowed to write to lo2s output path %s?", uid, _lo2s_trace_path);

    // if lo2s is not activated, we will send a CANCEL message to the daemon to stop it from writing to disk
    if (strlen(_lo2s_trace_path) == 0) {
        LOG("SPANK plugin lo2s: No output path set, sending CANCEL to daemon\n");
        FILE *pipe_file = open_fifo_for_write(pipe_path_write);
        if (!pipe_file) {
            LOG("SPANK plugin lo2s: No reader on pipe file %s (daemon gone), skipping CANCEL\n", pipe_path_write);
            CLOSE_LOG();
            return ESPANK_SUCCESS; // Do nothing if no path is set
        }
        fprintf(pipe_file, "CANCEL\n");
        fclose(pipe_file);
        return ESPANK_SUCCESS; // Do nothing if no path is set
    }


    // check if user is allowed to write to _lo2s_path, if not send an empty line to lo2s daemon to disable writing to disk
    if (strlen(_lo2s_trace_path) > 0) {
        // check whether uid is allowed to write to _lo2s_path, if not, disable writing to disk. Taht is not getuid() but the uid of the job, which is passed to us by spank_get_item
        // We need to check the permissions of the directory, not the file itself
        if (check_access(uid, gid, _lo2s_trace_path, log_file) != 1) {
            LOG("SPANK plugin lo2s: User %d is not allowed to write to lo2s output path %s, disabling writing to disk", uid, _lo2s_trace_path);
            slurm_error("SPANK plugin lo2s: User %d is not allowed to write to lo2s output path %s, disabling writing to disk", uid, _lo2s_trace_path);
            _lo2s_trace_path[0] = '\0';
        }
    }

    LOG("Access check done, writing to path %s\n", _lo2s_trace_path);
    if (strlen(_lo2s_trace_path) == 0) {
        LOG("SPANK plugin lo2s: No output path set, skipping\n");
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing if no path is set
    }

    LOG("Next steps\n");

    LOG("SPANK plugin lo2s:Correct output path used");
    LOG("SPANK plugin lo2s:SPANK init post opt in remote context\n");
    LOG("SPANK plugin lo2s:SPANK init post opt PID/PPID: %d/%d\n", getpid(), getppid());
    LOG("SPANK plugin lo2s:SPANK init post opt job info: uid=%d, gid=%d, jid=%d, stepid=%d\n", uid, gid, jid, stepid);

    // this is a FIFO file, we want to write to it. it is already created in the job prolog, so we can just open it for writing.
    // Use a non-blocking open so we do not hang if the daemon already exited (no reader).
    FILE *pipe_file = open_fifo_for_write(pipe_path_write);
    if (!pipe_file) {
        LOG("SPANK plugin lo2s: No reader on pipe file %s (daemon gone), skipping\n", pipe_path_write);
        CLOSE_LOG();
        return ESPANK_SUCCESS;
    }

    LOG( "And further on\n");

    char cgroup_path[3072] = {0};
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "/usr/bin/find /sys/fs/cgroup -name 'job_%d' | head -n 1", jid);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(cgroup_path, sizeof(cgroup_path), fp) != NULL) {
            cgroup_path[strcspn(cgroup_path, "\n")] = 0;
        }
        pclose(fp);
    }
    if (!cgroup_path[0]) {
        LOG("SPANK plugin lo2s: failed to find cgroup for job %d\n", jid);
        fclose(pipe_file);
        CLOSE_LOG();
        return ESPANK_SUCCESS;
    }

    LOG( "cgroup path: %s\n", cgroup_path);


    char hostname[1024];
    char buffer[4096];
    gethostname(hostname, sizeof(hostname));

    LOG( "hostname: %s\n", hostname);

    // Parent process: Write the lo2s options to the pipe
    // we will write "--verbose -A --cgroup %s -o $_lo2s_path/$jid/$hostname" to the pipe and ignore _lo2s_args
    snprintf(buffer, sizeof(buffer), "-q -A --dwarf full --cgroup %s -o %s/%d/%s", cgroup_path, _lo2s_trace_path, jid, hostname);

    if (strlen(_lo2s_event) > 0) {
        snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), " -e %s", _lo2s_event);
    } else {
        snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), " -e %s", "cpu-cycles");
    }
    if (_lo2s_event_rate > 0) {
        snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), " -c %d", _lo2s_event_rate);
    }
    // maybe we have multiple metrics comma separated, so we will add each of them with -E to the command line, so that lo2s can parse them correctly
    if (strlen(_lo2s_metrics) > 0) {
        // Split the metrics by comma and add each one with -E
        char *token = strtok(_lo2s_metrics, ",");
        while (token != NULL) {
            snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), " -E %s", token);
            token = strtok(NULL, ",");
        }
    }
    if (_lo2s_metrics_frequency > 0) {
        snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), " --metric-frequency %d", _lo2s_metrics_frequency);
    }
    if (_lo2s_standard_metrics > 0) {
        snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), " --standard-metrics");
    }

    fprintf(pipe_file, "%s\n", buffer);

    LOG( "sent options: %s %d\n", buffer, strlen(buffer));

    fclose(pipe_file);

    // write trac output path to _lo2s_cfg_output_path_template
    snprintf(buffer, sizeof(buffer), _lo2s_cfg_output_path_template, jid);
    FILE *output_path_file = fopen(buffer, "w");
    if (!output_path_file) {
        LOG("SPANK plugin lo2s: Failed to open output path file %s", buffer);
        CLOSE_LOG();
        return ESPANK_SUCCESS;
    }
    fprintf(output_path_file, "%s/%d\n", _lo2s_trace_path, jid);
    fclose(output_path_file);

    slurm_error("slurm_spank_init_post_opt: Done");

    CLOSE_LOG();
    return ESPANK_SUCCESS;
}

int cleanup(spank_t sp,  char * exit_function) {
    char hostname[1024];
    gethostname(hostname,sizeof(hostname));
    
    int jid, uid, gid, stepid;
    spank_get_item(sp, S_JOB_UID, &uid);
    spank_get_item(sp, S_JOB_GID, &gid);
    spank_get_item(sp, S_JOB_ID, &jid);
    spank_get_item(sp, S_JOB_STEPID, &stepid);
    int context = spank_context();

    OPEN_LOG(LO2S_EPILOG_DEBUG_PATH, jid, stepid, context, ESPANK_SUCCESS);

    // check if _lo2s_path exists, if not log and return
    if (access(_lo2s_cfg_path, F_OK) != 0) {
        LOG("%s: SPANK plugin lo2s: lo2s binary %s does not exist, skipping\n", exit_function, _lo2s_cfg_path);
        CLOSE_LOG();
        return ESPANK_SUCCESS; // Do nothing if lo2s binary does not exist
    }
    LOG("%s: lo2s path: %s\n", exit_function, _lo2s_trace_path);

    // Try to find the daemon PID: first from the cgroup, then from the PID file
    pid_t lo2s_pid = 0;
    char pid_file[3072];
    snprintf(pid_file, sizeof(pid_file), _lo2s_cfg_pid_path_template, jid);
    FILE *pf = fopen(pid_file, "r");
    if (pf) {
        char pid_str[16];
        if (fgets(pid_str, sizeof(pid_str), pf) != NULL) {
            lo2s_pid = (pid_t)strtol(pid_str, NULL, 10);
            if (lo2s_pid > 0) {
                LOG("%s: Found lo2s daemon PID %d in PID file %s\n", exit_function, lo2s_pid, pid_file);
            }
        }
        fclose(pf);
    }

    // unlink the pipe file, so that the lo2s daemon will exit
    char pipe_path_write[3072];
    snprintf(pipe_path_write, sizeof(pipe_path_write), _lo2s_cfg_comm_path_template, jid);
    // if the file exists, unlink it, otherwise log and continue
    if (access(pipe_path_write, F_OK) == 0) {     
        if (unlink(pipe_path_write) != 0) {
            LOG("%s: failed to unlink pipe file %s\n", exit_function, pipe_path_write);
        } else {
            LOG("%s: unlinked pipe file %s\n", exit_function, pipe_path_write);
        }
    } else {
        LOG("%s: pipe file %s does not exist\n", exit_function, pipe_path_write);
    }



    // got rid of files, now we will try to kill the daemon, if it exists.
    if (lo2s_pid > 0) {
        LOG("%s: Sending SIGINT to lo2s daemon PID %d\n", exit_function, lo2s_pid);
        // kill the lo2s daemon with SIGINT, so that it can clean up and exit gracefully
        if (kill(lo2s_pid, SIGINT) != 0) {
            LOG("%s: SIGINT to lo2s daemon PID %d failed (%d)\n", exit_function, lo2s_pid, errno);
        }

        // wait for the lo2s daemon to exit gracefully (building calling context
        // trees can take a while); timeout is configurable via LO2S_SHUTDOWN_TIMEOUT_MS
        int wait_time = 0;
        int max_wait_time = _lo2s_cfg_shutdown_timeout_ms / 100; // 100ms per iteration
        while (wait_time < max_wait_time) {
            if (kill(lo2s_pid, 0) != 0) {
                LOG("%s: lo2s daemon PID %d exited gracefully or we cannot signal it (%d)\n", exit_function, lo2s_pid, errno);
                break;
            }
            // check whether the process is still in /proc
            char proc_path[256];
            snprintf(proc_path, sizeof(proc_path), "/proc/%d", lo2s_pid);
            if (access(proc_path, F_OK) != 0) {
                LOG("%s: lo2s daemon PID %d exited gracefully (not in /proc)\n", exit_function, lo2s_pid);
                break;
            }
            usleep(100*1000); // sleep for 100ms
            wait_time++;
        }
        if (wait_time >= max_wait_time) {
            LOG("%s: lo2s daemon PID %d did not exit gracefully, sending SIGKILL\n", exit_function, lo2s_pid);
            if (kill(lo2s_pid, SIGKILL) != 0) {
                LOG("%s: failed to send SIGKILL to lo2s daemon PID %d\n", exit_function, lo2s_pid);
            }
        }
    } else {
        LOG("%s: No lo2s daemon PID found, skipping kill\n", exit_function);
    }


    LOG("%s: Cleaning up PID file %s\n", exit_function, pid_file);
    unlink(pid_file);

    // get the output path from the output path file
    char output_path_file[3072];
    snprintf(output_path_file, sizeof(output_path_file), _lo2s_cfg_output_path_template, jid);
    if (access(output_path_file, F_OK) == 0) {
        FILE *opf = fopen(output_path_file, "r");
        if (opf) {
            char output_path[3072];
            if (fgets(output_path, sizeof(output_path), opf) != NULL) {
                output_path[strcspn(output_path, "\n")] = 0;
                char buffer[4096];
                snprintf(buffer, sizeof(buffer), "%s/%s", output_path, hostname);
                LOG("%s: Changing ownership of lo2s output path %s to uid %d and gid %d\n", exit_function, output_path, uid, gid);
                // per host: recursive
                recursive_chown(uid, gid, buffer, log_file, 1);
                // overall not recursive (already done multiple times)
                recursive_chown(uid, gid, output_path, log_file, 0);

                // count the size of the output path, including all sub files and print size to LOG
                char du_cmd[4096];
                snprintf(du_cmd, sizeof(du_cmd), "du -sh %s", output_path);
                FILE *du_fp = popen(du_cmd, "r");
                if (du_fp) {
                    char du_output[1024];
                    if (fgets(du_output, sizeof(du_output), du_fp) != NULL) {
                        LOG("%s: Size of output path %s is %s", exit_function, output_path, du_output);
                    }
                    pclose(du_fp);
                }
            }
            fclose(opf);
        }
        // cleanup path file
        if (unlink(output_path_file) != 0) {
            LOG("%s: failed to unlink output path file %s\n", exit_function, output_path_file);
        } else {
            LOG("%s: unlinked output path file %s\n", exit_function, output_path_file);
        }
    } else {
        LOG("%s: output path file %s does not exist\n", exit_function, output_path_file);
    }


    // Clean up the cgroup: remove our lo2s_daemon sub-cgroup, then try to
    // remove the job cgroup. The job cgroup may not be empty (other processes
    // still running), which is fine — Slurm will retry after our exit.
    // If our rmdir fails here, the epilog will try again.
    char cgroup_path[3072] = {0};
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "/usr/bin/find %s -name 'job_%d' | head -n 1", _lo2s_cfg_cgroup_folder, jid);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(cgroup_path, sizeof(cgroup_path), fp) != NULL) {
            cgroup_path[strcspn(cgroup_path, "\n")] = 0;
        }
        pclose(fp);
    }
    if (cgroup_path[0]) {
        char lo2s_cgroup_path[3072];
        snprintf(lo2s_cgroup_path, sizeof(lo2s_cgroup_path), "%s/lo2s_daemon", cgroup_path);
        if (rmdir(lo2s_cgroup_path) == 0) {
            LOG("%s: Removed cgroup directory %s\n", exit_function, lo2s_cgroup_path);
        } else {
            LOG("%s: cgroup directory %s not present or not empty (errno=%d)\n", exit_function, lo2s_cgroup_path, errno);
        }
        // Try to remove the job cgroup (best-effort; may fail if not empty)
        if (rmdir(cgroup_path) == 0) {
            LOG("%s: Removed job cgroup %s\n", exit_function, cgroup_path);
        } else {
            LOG("%s: Job cgroup %s not empty, leaving for Slurm (errno=%d)\n", exit_function, cgroup_path, errno);
        }
    } else {
        LOG("%s: cgroup for job %d not found, skipping cgroup cleanup\n", exit_function, jid);
    }
    CLOSE_LOG();
    return ESPANK_SUCCESS;

}

// we need this at the end of task exit for the extern task, which should be the last to end
int slurm_spank_exit(spank_t sp, int ac, char **av) {
        int jid, uid, gid, stepid;
    spank_get_item(sp, S_JOB_ID, &jid);
    spank_get_item(sp, S_JOB_STEPID, &stepid);
    if (stepid != SLURM_EXTERN_CONT) {
        return ESPANK_SUCCESS; // Do nothing in step context
    }

    int context = spank_context();
    return cleanup(sp, "slurm_spank_exit extern step");
}

int slurm_spank_job_epilog(spank_t sp, int ac, char **av) {
    
    return cleanup(sp, "slurm_spank_job_epilog");
}




static int check_access(uid_t uid, gid_t gid, const char *path, FILE* log_file) {
    struct stat st;

    LOG("Get stat\n");
    // Get the directory's metadata
    if (stat(path, &st) != 0) {
        perror("stat failed");
        return -1;
    }

    LOG("Check UID\n");
    // Check ownership
    if (st.st_uid == uid) {
        // Owner permissions
        if (st.st_mode & S_IRUSR && st.st_mode & S_IXUSR) {
            return 1; // Read and execute access
        }
    } else if (st.st_gid == gid) {
        // Group permissions
        if (st.st_mode & S_IRGRP && st.st_mode & S_IXGRP) {
            return 1; // Read and execute access
        }
    } else {
        // Other permissions
        if (st.st_mode & S_IROTH && st.st_mode & S_IXOTH) {
            return 1; // Read and execute access
        }
    }
    LOG("No access\n");

    return 0; // No access
}


/**
 * changes ownership of a path with all its subdirectories and files recursively to the given uid and gid
 */
static int recursive_chown(uid_t uid, gid_t gid, char* path, FILE* log_file, int recursive){
    struct stat st;
    if (lstat(path, &st) != 0) {
        LOG("lstat failed on %s", path);
        return -1;
    }

    // Change ownership of the current path
    if (chown(path, uid, gid) != 0) {
        LOG("chown failed on %s", path);
        return -1;
    }

    // If it's a directory, recurse into it
    if (S_ISDIR(st.st_mode) && recursive) {
        LOG("Recursively changing ownership of %s\n", path);
        DIR *dir = opendir(path);
        if (!dir) {
            LOG("opendir failed on %s", path);
            return -1;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            // Skip "." and ".."
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            // Construct the full path for the entry
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

            // Recurse into the entry
            if (recursive_chown(uid, gid, full_path, log_file, recursive) != 0) {
                closedir(dir);
                return -1; // Propagate error
            }
        }
        closedir(dir);
    }
    return 0; // Success
}
