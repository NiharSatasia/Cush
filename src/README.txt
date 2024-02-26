Student Information
-------------------
Nihar Satasia - niharsatasia
Patrick Walsh - walsh968

How to execute the shell
------------------------
To execute the shell you first need to navigate to the posix_spawn directory and run 'make' to build the posix_spawn library. 
Then navigate to the src directory and run 'make' which will compile the program. Afterwards while still in the src directory 
running './cush' in the terminal will correctly execute the shell.

Important Notes
---------------
<Any important notes about your system>

Added #include <limits.h>, #include <fnctl.h>, and #include<readline/history.h>

Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

jobs: If the first command line argument matches "jobs" then we traverses through the list of all jobs and 
prints out each job out as long as its status is not equal to foreground.

fg: If the first command line argument matches "fg", then we create a new job struct based on the jid which will be the 
second command line argument (used atoi to convert from string to int and get_job_from_jid(jid) to create job). We first 
give the terminal to the job and if the job is already stopped then we send a SIGCONT signal using killpg() and continue it. 
We then set the job's status to FOREGROUND, print the info out, and wait for the job to complete. Finally, we give ownership of 
the terminal back to the shell.

bg: If the first command line argument matches "bg", then again we create a new job based on the jid which will be the 
second command line argument (used atoi to convert from string to int and get_job_from_jid(jid) to create job). Finally, 
if the job's status is not BACKGROUND then we send a SIGCONT using killpg(), conitnue it, and set the job's status to BACKGROUND.

kill: Similar to fg and bg, if the first command line argument matches "kill", then we create a new job based on the jid which
wll be the second command line argument (used atoi to convert from string to int and get_job_from_jid(jid) to create job). Then,
we simply terminate the job by sending a SIGTERM signal using killpg().

stop: Exactly like the kill implementation except we check for the first command line argument to match "stop" and we send a 
SIGSTOP signal using killpg() instead of SIGTERm.

\^C: This functionality is handled within handle_child_status. If the user enters ctrl-c we then send a WIFSIGNALLED signal. If the
WTERMSIG(status) is equal to SIGINT, then we decrement the num_processes_alive and set that job's status to DEAD. We also handle the 
cases where a user terminates the process via kill, kill -9, or the general case in which we do the same exact t hing but also print
out the strsignal.

\^Z: This functionality is also handeled within handle_child_status. If the user enters ctrl-z we then send a WIFSTOPPED signal. If
the WSTOPSIG(status) is equal to SIGTSTP, then we first save the terminal state, then set the job status to STOPPED, print out the job, 
and finally give the terminal back to the shell. We also handle the other WSTOPSIG(status) that euqal either SIGSTOP or SIGTTOU/SIGTTIN.
For SIGSTOP we do the saem exact thing but do not print out the job. For SIGTTOU/SIGTTIN we check to see if the job status is not equal
to FOREGROUND and if it is not then we set the status to NEEDSTERMINAL.

Description of Extended Functionality
-------------------------------------
<describe your IMPLEMENTATION of the following functionality:
I/O, Pipes, Exclusive Access >

I/O: We implemented I/O redirection by using the struct ast_pipeline and specifically the fields iored_input iored_output, and 
append_to_output. They allowed us to check if an input is coming in or if it was going out. If it is coming in then we use 
posix_spawn_file_actions_addopen() to open it and give it read only permissions. If it is going out then we check to see if 
append_to_output is true and if it is then we use posix_spawn_file_actions_addopen() to open it and give it write only permissions
along with the append flag. If it is not true then we use posix_spawn_file_actions_addopen() to open it and give it write only along
with the create and truncate flag.

Pipes: We implemented our pipes using pipe2(), posix_spawn_file_actions_adddup2(), and posix_spawn_file_actions_addclose. 
The process starts by redirecting the first command to write to the pipe using STDOUT_FILENO, then the middle commands to read
from one pipe and write to another using STDIN_FILENO and STDOUT_FILENO, and finally the last command to read from the pipe using
STDIN_FILENO. Posix spawn executes the commands and duplicates the correct file descriptors to the input/output as needed. Finally,
we made sure to close the pipes after we were done using them.

Exclusive Access: To ensure exlusive access, we made sure to use termstate religiously. specifically we used termstate_save(), 
termstate_sample(), and termstate_give_terminal_back_to_shell(). We used termstate_save() to save the terminal state when a
job was stopped, and when running BACKGROUND jobs. We used termstate_sample() to sample the terminal state whenever a FOREGROUND job
was stopped. We used termstate_give_terminal_back_to_shell() to give the terminal back to the shell after either giving it to a job or
saving the terminal state. This was all mainly implemented within handle_child_status and the fg built-in.

List of Additional Builtins Implemented
---------------------------------------
(Written by Your Team)

cd: The cd command is designed to change directories as the user desires. We created a utility function called update_directory to 
accomplish this correctly. The function takes in a desired directory called 'new_dir', allcoates memory for a temp directory using 
malloc, changes to the new directory using chdir(), stores the current dirrectory using getcwd(), and finally updates the 
previous directory to be the current directory and the current directory to be the temp directory. This function allows us to pass
in the second command line agurment as the new directory and correctly change to it. If the user does not eneter a second command line
argument then we simply change to the home directory using getenv("HOME"). The final case of a user entering 'cd -' which will change
to the previous directory is also correctly handled by update_directory by keeping track of directories via pointers. 

history: The history command is designed to print out all of the commands the user has entered during their session. We
implemented it by using history(3). We first initialized the history list, then looped through it all until the index position 
was undefined and printed out each entry position along with the command that was entered. We were able to easily do this because
the history(3) readline library allows us to use add_history(cmdline) which keeps track of all commands entered and adds them to the list.