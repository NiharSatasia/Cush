/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <fcntl.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
#include <spawn.h>
#include <readline/history.h>
#include <limits.h>

static void handle_child_status(pid_t pid, int status);
static struct job *get_job_from_pid(pid_t pid);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
           " -h            print this help\n",
           progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status
{
    FOREGROUND,    /* job is running in foreground.  Only one job can be
                      in the foreground state. */
    BACKGROUND,    /* job is running in background */
    STOPPED,       /* job is stopped via SIGSTOP */
    NEEDSTERMINAL, /* job is stopped because it was a background job
                      and requires exclusive terminal access */
};

struct job
{
    struct list_elem elem;          /* Link element for jobs list. */
    struct ast_pipeline *pipe;      /* The pipeline of commands this job represents */
    int jid;                        /* Job id. */
    enum job_status status;         /* Job status. */
    int num_processes_alive;        /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was
                                       stopped after having been in foreground */

    /* Add additional fields here if needed. */
    pid_t pid;            /* Process id of the job */
    struct list pid_list; /* List of PIDs associated with the job */
    pid_t pgid;           /* Process group id of the job */
};

// Struct for jobs that contain multiple processes
struct pid_mult
{
    pid_t pid2;
    struct list_elem mult_elem;
};

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1 << 16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job *job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    // Initalize job list
    list_init(&job->pid_list);
    for (int i = 1; i < MAXJOBS; i++)
    {
        if (jid2job[i] == NULL)
        {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status)
    {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e))
    {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD
 * signal may be delivered for multiple children that have
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 *
 * Implement handle_child_status such that it records the
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0)
    {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented.
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */

    // Updated to save terminal states when needed
    struct job *job = get_job_from_pid(pid);
    if (WIFEXITED(status))
    {
        // fprintf(stderr, "\nProcess %d terminated by signal: %s\n", pid, strsignal(WTERMSIG(status)));
        job->num_processes_alive--;
        if (job->status == FOREGROUND)
        {
            // Sample the current terminal state because a foreground process exited
            termstate_sample();
        }
    }
    // ^C
    else if (WIFSIGNALED(status))
    {
        fprintf(stderr, "Process %d terminated by signal: %s\n", pid, strsignal(WTERMSIG(status)));
        job->num_processes_alive--;
    }
    else if (WIFSTOPPED(status))
    {
        // ^Z
        if (WSTOPSIG(status) == SIGTSTP)
        {
            // fprintf(stderr, "Process %d stopped by signal: %s\n", pid, strsignal(WSTOPSIG(status)));
            // Save the terminal state because a process was stopped
            termstate_save(&job->saved_tty_state);
            job->status = STOPPED;
            print_job(job);
        }
        if (WSTOPSIG(status) == SIGSTOP)
        {
            // fprintf(stderr, "Process %d stopped by signal: %s\n", pid, strsignal(WSTOPSIG(status)));
            // Save the terminal state because a process was stopped
            termstate_save(&job->saved_tty_state);
            job->status = STOPPED;
        }
        if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN)
        {
            // fprintf(stderr, "Process %d needs terminal to continue (stopped by signal: %s)\n", pid, strsignal(WSTOPSIG(status)));
            job->status = NEEDSTERMINAL;
        }
    }
}

// Utility function to find job based on pid, updated to handle jobs with multiple processes
static struct job *get_job_from_pid(pid_t pid)
{
    // Iterarte through job list
    struct list_elem *job_elem;
    for (job_elem = list_begin(&job_list); job_elem != list_end(&job_list); job_elem = list_next(job_elem))
    {
        struct job *current_job = list_entry(job_elem, struct job, elem);
        struct list_elem *pid_elem;
        // Iterate through processes associated with the current job
        for (pid_elem = list_begin(&current_job->pid_list); pid_elem != list_end(&current_job->pid_list); pid_elem = list_next(pid_elem))
        {
            struct pid_mult *current_pid_elem = list_entry(pid_elem, struct pid_mult, mult_elem);
            pid_t current_pid = current_pid_elem->pid2;
            // Check if the current process's pid matches the given pid
            if (current_pid == pid)
            {
                return current_job;
            }
        }
    }
    // Return NULL if no jobs matches pid
    return NULL;
}

// Utility function to update the directory using 'cd' built-in especially for 'cd -'
char *prev_dir = NULL;
char *current_dir = NULL;
static void update_directory(const char *new_dir)
{
    // Allocate memory new directory
    char *temp_dir = malloc(PATH_MAX);
    // Change directory
    chdir(new_dir);
    // Store current directory in temp_dir
    getcwd(temp_dir, PATH_MAX);
    // Update pointers
    prev_dir = current_dir;
    current_dir = temp_dir;
}

int main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0)
    {
        switch (opt)
        {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;)
    {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        // termstate_give_terminal_back_to_shell();
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        // delete job do anywhere between here and where we spawn the processes (after ast_commandlineprint(cline))
        free(prompt);

        if (cmdline == NULL) /* User typed EOF */
            break;

        // Tracking history
        add_history(cmdline);

        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        free(cmdline);
        if (cline == NULL) /* Error in command line */
            continue;

        if (list_empty(&cline->pipes))
        { /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        signal_block(SIGCHLD);

        // Iterate over each pipeline
        struct list_elem *pipe_elem;
        for (pipe_elem = list_begin(&cline->pipes); pipe_elem != list_end(&cline->pipes); pipe_elem = list_next(pipe_elem))
        {
            struct ast_pipeline *pipeline = list_entry(pipe_elem, struct ast_pipeline, elem);
            // Current job user types in
            struct job *job = add_job(pipeline);

            // Iterate over each command in the pipeline
            struct list_elem *cmd_elem;
            for (cmd_elem = list_begin(&pipeline->commands); cmd_elem != list_end(&pipeline->commands); cmd_elem = list_next(cmd_elem))
            {
                struct ast_command *cmd = list_entry(cmd_elem, struct ast_command, elem);

                // Implementing built-in commands
                if (strcmp(cmd->argv[0], "jobs") == 0)
                {
                    for (struct list_elem *e = list_begin(&job_list); e != list_end(&job_list); e = list_next(e))
                    {
                        struct job *jobs = list_entry(e, struct job, elem);
                        if (jobs->status != FOREGROUND)
                        {
                            print_job(jobs);
                        }
                    }
                }
                else if (strcmp(cmd->argv[0], "exit") == 0)
                {
                    // Working as intended
                    exit(0);
                }
                else if (strcmp(cmd->argv[0], "fg") == 0)
                {
                    // Use either atoi or strol per discord
                    // Converting string to int
                    int jid = atoi(cmd->argv[1]);
                    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
                    {
                        struct job *job = get_job_from_jid(jid);
                        if (job)
                        {
                            // Give the terminal to the job
                            termstate_give_terminal_to(&job->saved_tty_state, job->pgid);
                            // Continue job if it was stopped (accounts for user ^Z)
                            if (job->status == STOPPED)
                            {
                                killpg(job->pgid, SIGCONT);
                            }
                            // Set status to foreground
                            job->status = FOREGROUND;
                            // Print out info to terminal (for tests)
                            print_cmdline(job->pipe);
                            printf("\n");
                            wait_for_job(job);
                            // Give the terminal back to the shell
                            termstate_give_terminal_back_to_shell();
                        }
                    }
                }
                else if (strcmp(cmd->argv[0], "bg") == 0)
                {
                    // Converting string to int
                    int jid = atoi(cmd->argv[1]);
                    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
                    {
                        struct job *job = get_job_from_jid(jid);
                        // Continue job if it was stopped (accounts for user ^Z)
                        if (job && job->status == STOPPED)
                        {
                            killpg(job->pgid, SIGCONT);
                            // Set status to background
                            job->status = BACKGROUND;
                        }
                    }
                }
                else if (strcmp(cmd->argv[0], "kill") == 0)
                {
                    // Converting string to int
                    int jid = atoi(cmd->argv[1]);
                    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
                    {
                        struct job *job = get_job_from_jid(jid);
                        if (job)
                        {
                            // Terminate job
                            killpg(job->pgid, SIGTERM);
                        }
                    }
                }
                else if (strcmp(cmd->argv[0], "stop") == 0)
                {
                    // Converting string to int
                    int jid = atoi(cmd->argv[1]);
                    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
                    {
                        struct job *job = get_job_from_jid(jid);
                        if (job)
                        {
                            // Stop job
                            killpg(job->pgid, SIGSTOP);
                        }
                    }
                }
                else if (strcmp(cmd->argv[0], "cd") == 0)
                {
                    if (cmd->argv[1] != NULL)
                    {
                        if (strcmp(cmd->argv[1], "-") == 0)
                        {
                            // Calling utility function for 'cd -' case
                            if (prev_dir)
                            {
                                update_directory(prev_dir);
                                printf("%s\n", current_dir);
                            }
                        }
                        update_directory(cmd->argv[1]);
                    }
                    else
                    {
                        // Go to home directory if user does not specify
                        update_directory(getenv("HOME"));
                    }
                }
                else if (strcmp(cmd->argv[0], "history") == 0)
                {
                    // Referenced https://linux.die.net/man/3/history
                    // History list
                    HIST_ENTRY **the_history_list = history_list();
                    int i = 0;
                    // Loop through list and print (entry number, command)
                    while (the_history_list[i] != NULL)
                    {
                        // history_base is entry position stored in zero based index
                        int entry = history_base + i;
                        // 'line' contains the command string
                        char *command = the_history_list[i]->line;
                        printf("    %d %s\n", entry, command);
                        i++;
                    }
                }
                else
                {
                    // Handling non built in functions
                    struct list_elem *e = list_begin(&pipeline->commands);
                    // Ensure there is at least one command
                    if (e != list_end(&pipeline->commands))
                    {
                        // struct ast_command *cmd = list_entry(e, struct ast_command, elem);

                        pid_t pid;
                        posix_spawn_file_actions_t file;
                        posix_spawn_file_actions_init(&file);
                        printf("Executing command: %s\n", cmd->argv[0]);
                        // check to see if input is coming from anywhere or output is going somewhere
                        if (pipeline->iored_input || pipeline->iored_output)
                        {
                            // posix_spawn_file_actions_t file;
                            // posix_spawn_file_actions_init(&file);
                            if (pipeline->iored_input)
                            {
                                posix_spawn_file_actions_addopen(&file, STDIN_FILENO, pipeline->iored_input, O_RDONLY, 0666);
                            }
                            if (pipeline->iored_output)
                            {
                                if (pipeline->append_to_output)
                                {
                                    // open(pipeline->iored_output, O_WRONLY | O_APPEND | O_CREAT, 0644);
                                    posix_spawn_file_actions_addopen(&file, STDOUT_FILENO, pipeline->iored_output, O_WRONLY | O_APPEND | O_CREAT, 0644);
                                }
                                else
                                {
                                    // open(pipeline->iored_output, O_WRONLY | O_TRUNC | O_CREAT, 0644);
                                    posix_spawn_file_actions_addopen(&file, STDOUT_FILENO, pipeline->iored_output, O_WRONLY | O_TRUNC | O_CREAT, 0644);
                                }
                                posix_spawn_file_actions_addopen(&file, STDOUT_FILENO, pipeline->iored_output, O_WRONLY, 0666);
                            }
                        }

                        posix_spawnattr_t attr;
                        posix_spawnattr_init(&attr);

                        if (pipeline->bg_job)
                        {
                            job->status = BACKGROUND;
                            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK);
                        }

                        if (!pipeline->bg_job)
                        {
                            job->status = FOREGROUND;
                            // Using 0x100 instead of 'POSIX_SPAWN_TCSETPGROUP' per forum post
                            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK | 0x100);
                        }

                        // wire up pipe -- currently wrong
                        // if not last, wire up pipe output
                        int fds[2];
                        pipe2(fds, O_CLOEXEC);
                        if (list_next(cmd_elem) != list_end(&pipeline->commands))
                        {
                            posix_spawn_file_actions_adddup2(&file, fds[0], STDOUT_FILENO);
                        }
                        // if not first wire up pipe input
                        if (cmd_elem != list_begin(&pipeline->commands))
                        {
                            posix_spawn_file_actions_adddup2(&file, fds[1], STDIN_FILENO);
                        }

                        if (posix_spawnp(&pid, cmd->argv[0], &file, &attr, cmd->argv, environ) != 0)
                        {
                            perror("spawn failed");
                        }
                        else
                        {
                            if (job)
                            {
                                // Initalize pid of job
                                struct pid_mult *job_pid = malloc(sizeof(struct pid_mult));
                                job_pid->pid2 = pid;
                                // Add to end of pid list
                                list_push_back(&job->pid_list, &job_pid->mult_elem);
                                // Set pgid
                                job->pgid = pid;
                                // Update process count
                                job->num_processes_alive++;
                                // Print out info if background job
                                if (job->status == BACKGROUND)
                                {
                                    printf("[%d] %d\n", job->jid, pid);
                                    termstate_save(&job->saved_tty_state);
                                }
                            }
                            else if (job->num_processes_alive == 0)
                            {
                                list_remove(e);
                                delete_job(job);
                            }
                        }
                    }
                }
            }
            wait_for_job(job);
            signal_unblock(SIGCHLD);
            termstate_give_terminal_back_to_shell();
        }

        // ast_command_line_print(cline); /* Output a representation of
        //                                   the entered command line */

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        //  ast_command_line_free(cline);
    }
    return 0;
}
