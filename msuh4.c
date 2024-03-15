#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h> /* For errno after readLongString */
#include <fcntl.h> 
#include <mush.h>

#define False 0
#define True 1

void handle_cd_command(char **argv, int argc);
void run_command(char* cmd);
void process_input(FILE* input);
void sigint_handler(int sig);

volatile sig_atomic_t sigint_received = False;
int batch = False;

/** Assuming readLongString is implemented as described **/

void sigint_handler(int sig) {
    sigint_received = True;
    /** Do not call process_input(stdin) here directly to avoid recursion **/
}

void handle_cd_command(char **argv, int argc) {
    /** stage->argc //stage->argv **/
    char *dir = argc > 1 ? argv[1] : getenv("HOME");
    if (!dir) {
        struct passwd *pw = getpwuid(getuid());
        dir = pw ? pw->pw_dir : NULL;
    }
    if (dir && chdir(dir) != 0) {
        perror("chdir");
    } else if (!dir) {
        fprintf(stderr, "unable to determine home directory\n");
    }
}

void process_input(FILE* input) {
    char* line = NULL;
    while (!feof(input) && !ferror(input)) {
        errno = 0; /* Clear errno before calling readLongString */
        line = readLongString(input);
        if (line == NULL) {
            if (errno == EINTR && sigint_received) {
                if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && !batch) {
                    printf("\n8-P ");
                    fflush(stdout);
                }
                sigint_received = False;
                continue;
            } else if (!feof(input)) {
                perror("readLongString");
            }
            break;
        }
        
        run_command(line);
        free(line); /* Ensure to free the memory allocated by readLongString */
        if (!batch && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            printf("8-P "); /* Print prompt again in interactive mode */
            fflush(stdout);
        }
    }
}

void run_command(char* cmd) {
    pipeline cl = crack_pipeline(cmd);
    if (cl == NULL) {
        switch (clerror) {
            case E_NONE:
                break;
            case E_NULL:
                fprintf(stderr, "Error: Invalid null command.\n");
                break;
            case E_EMPTY:
                /** Ignore empty commands **/
                return; 
            case E_BADIN:
                fprintf(stderr, "Error: Ambiguous input redirection.\n");
                break;
            case E_BADOUT:
                fprintf(stderr, "Error: Ambiguous output redirection.\n");
                break;
            case E_BADSTR:
                fprintf(stderr, "Error: Unterminated string detected.\n");
                break;
            case E_PARSE:
                fprintf(stderr, "Error: Generic parse error.\n");
                break;
            default:
                fprintf(stderr, "Error: Unknown parsing error.\n");
                break;
        }
        
        return;
    }

    /** Debug: Print the parsed pipeline **/
    /**print_pipeline(stdout, cl);**/
    

    int prev_fd = STDIN_FILENO; /* first command, input stdin */
    int fd[2]; 
    int i;

    for (i = 0; i < cl->length; i++) {
        clstage stage = &(cl->stage[i]);

        /**cd**/
        if (strcmp(stage->argv[0], "cd") == 0) { 
            if (cl->length == 1) { 
                handle_cd_command(stage->argv, stage->argc);
            } else {
                fprintf(stderr, "'cd' cannot be part of a pipeline\n");
            }
            continue;
        }

        
        if (i < cl->length - 1) { /* Not last, set pipe */
            if (pipe(fd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
        } else {
            fd[0] = -1; /* Not used */
            fd[1] = STDOUT_FILENO; /* Last command, output to stdout */
        }

        pid_t pid = fork();
        if (pid == 0) { /* Child */
           
            if (stage->inname) {
                int in_fd = open(stage->inname, O_RDONLY);
                if (in_fd == -1) {
                    perror("child process open input file");
                    exit(EXIT_FAILURE);
                }
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            } else if (prev_fd != STDIN_FILENO) {
                dup2(prev_fd, STDIN_FILENO); 
                /** output from the prev **/
            }

            
            if (stage->outname) {
                int out_fd = open(stage->outname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                dup2(out_fd, STDOUT_FILENO);
                close(out_fd);
            } else if (fd[1] != STDOUT_FILENO) {
                dup2(fd[1], STDOUT_FILENO); /* next command in pipe */
            }

            
            if (fd[0] != -1) close(fd[0]);

            
            execvp(stage->argv[0], stage->argv);
            perror("execvp");
            exit(EXIT_FAILURE);
        } else if (pid > 0) { /* Parent */
            wait(NULL); /* Wait for child */
            if (prev_fd != STDIN_FILENO) close(prev_fd); 
            if (fd[1] != STDOUT_FILENO) close(fd[1]); 

            prev_fd = fd[0]; /* Next command reads from here */
        } else {
            perror("fork");
            exit(EXIT_FAILURE);
        }
    }
     yylex_destroy();

    free_pipeline(cl);
}


int main(int argc, char* argv[]) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* restart system calls */
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    if (argc == 1) {
        batch = False;
        if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            printf("8-P "); 
            fflush(stdout);
        }
        process_input(stdin);
    } else if (argc == 2) {
        batch = True;
        FILE *file = fopen(argv[1], "r");
        if (!file) {
            perror("fopen");
            exit(EXIT_FAILURE);
        }
        process_input(file);
        fclose(file);
    } else {
        fprintf(stderr, "Usage: %s [file]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    return 0;
}
