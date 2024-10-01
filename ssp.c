#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define INITIAL_SIZE 10

struct ssp_process {
    int status;
    int ssp_pid;
    pid_t pid;
    char *name;
};

struct ssp_process *ssp_processes = NULL;   
int ssp_process_count = 0;
int maxSize = INITIAL_SIZE;

struct ssp_process *ssp_orphans = NULL;   
int ssp_orphans_count = 0;
int maxSizeOrphans = INITIAL_SIZE;

void handle_signal(int signum) {
    if (signum == SIGCHLD) {
        int status;
        pid_t result;

        // Loop to handle multiple children terminating at the same time
        while ((result = waitpid(-1, &status, WNOHANG)) > 0) {
            // Check if the process is already tracked
            bool found = false;
            for (int i = 0; i < ssp_process_count; i++) {
                if (ssp_processes[i].pid == result) {
                    found = true;
                    // Update the status for tracked processes
                    if (WIFEXITED(status)) {
                        ssp_processes[i].status = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        ssp_processes[i].status = WTERMSIG(status) + 128;
                    }
                    break;
                }
            }

            // If the process is not found, it's an orphan
            if (!found) {
                // Check if the orphan list is full
                if (ssp_orphans_count == maxSizeOrphans) {
                    maxSizeOrphans *= 2;
                    ssp_orphans = realloc(ssp_orphans, maxSizeOrphans * sizeof(struct ssp_process));
                    if (ssp_orphans == NULL) {
                        int err = errno;
                        perror("realloc failed");
                        exit(err);
                    }
                }

                // Add the orphan process to the list with name "<unknown>"
                ssp_orphans[ssp_orphans_count].pid = result;
                ssp_orphans[ssp_orphans_count].ssp_pid = ssp_orphans_count;
                ssp_orphans[ssp_orphans_count].name = strdup("<unknown>");
                
                if (WIFEXITED(status)) {
                    ssp_orphans[ssp_orphans_count].status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    ssp_orphans[ssp_orphans_count].status = WTERMSIG(status) + 128;
                }
                
                ssp_orphans_count++;
            }
        }

        // Handle waitpid failure
        if (result == -1 && errno != ECHILD) {
            int err = errno;
            perror("waitpid failed");
            exit(err);
        }
    }
}

void register_signal(int signum) {
    struct sigaction new_action = {0};
    sigemptyset(&new_action.sa_mask);
    new_action.sa_handler = handle_signal;
    new_action.sa_flags = SA_RESTART | SA_NOCLDSTOP;  // Restart syscalls and ignore stopped child processes
    if (sigaction(signum, &new_action, NULL) == -1) {
        int err = errno;
        perror("sigaction failed");
        exit(err);
    }
}

void ssp_init() {
    // Allocate memory for the process list
    ssp_processes = malloc(maxSize * sizeof(struct ssp_process));
    if (ssp_processes == NULL) {
        int err = errno;
        perror("malloc failed");
        exit(err);
    }

    // Allocate memory for the orphan list
    ssp_orphans = malloc(maxSizeOrphans * sizeof(struct ssp_process));
    if (ssp_orphans == NULL) {
        int err = errno;
        perror("malloc failed");
        exit(err);
    }

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) == -1) {
        int err = errno;
        perror("prctl failed");
        exit(err);
    }

    ssp_process_count = 0;
    ssp_orphans_count = 0;

    // Register the SIGCHLD signal handler
    register_signal(SIGCHLD);
}

int ssp_create(char *const *argv, int fd0, int fd1, int fd2) {
    // Check if the process list is full
    if (ssp_process_count == maxSize) {
        maxSize *= 2;
        ssp_processes = realloc(ssp_processes, maxSize * sizeof(struct ssp_process));
        if (ssp_processes == NULL) {
            int err = errno;
            perror("realloc failed");
            exit(err);
        }
    }

    // Create a new process
    pid_t pid = fork();

    // If fork fails 
    if (pid == -1) {
        int err = errno;
        perror("fork failed");
        exit(err);
    }

    // If fork succeeds
    // In child process
    if (pid == 0) {
        // Redirect stdin, stdout, stderr
        if (dup2(fd0, 0) == -1 || dup2(fd1, 1) == -1 || dup2(fd2, 2) == -1) {
            int err = errno;
            perror("dup2 failed");
            exit(err);
        }

        // Open /proc/self/fd
        DIR *dir = opendir("/proc/self/fd");
        if (dir == NULL) {
            int err = errno;
            perror("opendir failed");
            exit(err);
        }

        // Close all file descriptors except stdin, stdout, stderr
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_LNK) {
                int fd = atoi(entry->d_name);
                if (fd > 2 && fd != dirfd(dir)) {
                    close(fd);
                }
            }
        }

        // Close /proc/self/fd
        if (closedir(dir) == -1) {
            int err = errno;
            perror("closedir failed");
            exit(err);
        }

        // Execute the program
        execvp(argv[0], argv);

        // If execvp fails
        int err = errno;
        perror("execvp failed");
        exit(err);
    }

    // In parent process
    // Add the process to the list
    ssp_processes[ssp_process_count].status = -1;                   // set the status to running
    ssp_processes[ssp_process_count].pid = pid;                     // copy the pid
    ssp_processes[ssp_process_count].ssp_pid = ssp_process_count;   // set the ssp_pid
    ssp_processes[ssp_process_count].name = strdup(argv[0]);    // copy the string
    ssp_process_count++;

    return ssp_process_count - 1;
}

/*Returns -2 if the ssp_id is not valid*/
int ssp_get_status(int ssp_id) {
    // Check if the ssp_id is valid
    if (ssp_id < 0 || ssp_id >= ssp_process_count) {
        return -2;
    }

    struct ssp_process *proc = &ssp_processes[ssp_id];

    // Check if the status is already known
    if (proc->status != -1) {
        return proc->status;
    }

    // Check if the procces has terminated
    int status;
    pid_t result = waitpid(proc->pid, &status, WNOHANG);

    if (result == 0) {
        // Process is still running
        return -1;
    } else if (result == -1) {
        // Error in waitpid
        int err = errno;
        perror("waitpid failed");
        exit(err);
    }

    // Process has terminated
    if (WIFEXITED(status)) {
        proc->status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        proc->status = WTERMSIG(status) + 128;
    }

    return proc->status;
}

void ssp_send_signal(int ssp_id, int signum) {
    // Check if the ssp_id is valid
    if (ssp_id < 0 || ssp_id >= ssp_process_count) {
        return;
    }

    struct ssp_process *proc = &ssp_processes[ssp_id];

    // Check if the process is still running
    if(proc->status != -1) {
        return;
    }

    // Send the signal
    if(kill(proc->pid, signum) == -1) {
        // If the process has already terminated
        if(errno == ESRCH) {
            return;
        } else {
            // If kill fails
            int err = errno;
            perror("kill failed");
            exit(err);
        }
    }

    // Otherwise the signal has been sent
    return;    
}

void ssp_wait() {
    for(int i = 0; i < ssp_process_count; i++) {
        struct ssp_process *proc = &ssp_processes[i];

        // Check if the process is still running
        if(proc->status == -1) {
            int status;
            pid_t result = waitpid(proc->pid, &status, 0);

            // If there's an error in waitpid
            if (result == -1) {
                int err = errno;
                perror("waitpid failed");
                exit(err);
            }

            // Process has terminated
            if (WIFEXITED(status)) {
                proc->status = WEXITSTATUS(status); // exit code if process exited normally
            } else if (WIFSIGNALED(status)) {
                proc->status = WTERMSIG(status) + 128; // signal number if process was terminated by a signal
            }
        }
    }
}

void ssp_print() {
    // Find the longest name
    int max_len = strlen("CMD");
    for (int i = 0; i < ssp_process_count; i++) {
        int len = strlen(ssp_processes[i].name);
        if (len > max_len) {
            max_len = len;
        }
    }
    for(int i = 0; i < ssp_orphans_count; i++) {
        int len = strlen(ssp_orphans[i].name);
        if (len > max_len) {
            max_len = len;
        }
    }

    // Print the header
    printf("%7s %-*s %s\n", "PID", max_len, "CMD", "STATUS");

    // Print the processes
    for (int i = 0; i < ssp_process_count; i++) {
        struct ssp_process *proc = &ssp_processes[i];
        printf("%7d %-*s %d\n", proc->pid, max_len, proc->name, proc->status);
    }

    // Print the orphans
    for (int i = 0; i < ssp_orphans_count; i++) {
        struct ssp_process *proc = &ssp_orphans[i];
        printf("%7d %-*s %d\n", proc->pid, max_len, proc->name, proc->status);
    }
}
