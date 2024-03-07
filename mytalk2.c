#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <ctype.h>
#include "talk.h"
#include <ncurses.h>

#define BUFFER_SIZE 1024

int verbosity = 0;
int acceptConnectionsAutomatically = 0;
int disableWindowing = 0;

void runServer(int port);
void parseCommandLine(int argc, char *argv[], char **hostname, int *port);
void runClient(const char *hostname, int port);
void chatMode(int sockfd);

void error(const char *msg) {
    perror(msg);
    exit(1);
}

void parseCommandLine(int argc, char *argv[], char **hostname, int *port) {
    int opt;
    long portTmp;
    while ((opt = getopt(argc, argv, "vaN")) != -1) {
        switch (opt) {
            case 'v':
                verbosity++;
                break;
            case 'a':
                acceptConnectionsAutomatically = 1;
                break;
            case 'N':
                disableWindowing = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-V] [-a] [-N] [hostname] port\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    portTmp = strtol(argv[optind], NULL, 10);

    if (portTmp == 0 && optind < argc) {
        *hostname = argv[optind];
        if (optind + 1 < argc) {
            portTmp = strtol(argv[optind + 1], NULL, 10);
        } else {
            fprintf(stderr, "Port number not specified.\n");
            exit(EXIT_FAILURE);
        }
    }

    if (portTmp < 1024 || portTmp > 65535) {
        fprintf(stderr, "Error: Port number must be between 1025 and 65535.\n");
        exit(EXIT_FAILURE);
    }
    *port = (int) portTmp;
}


void runServer(int port) {
    int sockfd, newsockfd;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t clilen;
    char buffer[BUFFER_SIZE];
    char username[BUFFER_SIZE] = {0};
    const char *response = "ok\n";
    const char *deny = "Connection declined\n";

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) 
        error("ERROR on binding");

    listen(sockfd, 5);
    clilen = sizeof(cli_addr);

    while (1) {
        newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (newsockfd < 0) error("ERROR on accept");

        ssize_t bytes_read = recv(newsockfd, username, BUFFER_SIZE - 1, 0);
        if (bytes_read > 0) {
            username[bytes_read] = '\0';
        } else {
            close(newsockfd);
            continue;
        }

        fflush(stdin);
        printf("Mytalk request from %s@%s. Accept (y/n)? ", username, inet_ntoa(cli_addr.sin_addr));
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL || (strcasecmp(buffer, "y\n") != 0 && strcasecmp(buffer, "yes\n") != 0)) {
            write(newsockfd, deny, strlen(deny));
            close(newsockfd);
            continue;
        }

        write(newsockfd, response, strlen(response));
        chatMode(newsockfd);
        close(newsockfd);
    }
    close(sockfd);
}

void runClient(const char *hostname, int port) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct passwd *pw;
    char *username;

    pw = getpwuid(getuid());
    if (pw == NULL) {
        error("Failed to get user information");
    }
    username = pw->pw_name;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    server = gethostbyname(hostname);
    if (server == NULL) 
        error("ERROR, no such host");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
          (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);
    serv_addr.sin_port = htons(port);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    send(sockfd, username, strlen(username)+1, 0);

    chatMode(sockfd);

    close(sockfd);
}


void chatMode(int sockfd) {
    struct pollfd fds[2];
    char buffer[BUFFER_SIZE + 1];
    int endSession = 0;

    if (!disableWindowing) {
        start_windowing();
    }

    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = sockfd;
    fds[1].events = POLLIN;

    while (!endSession) {
        if (poll(fds, 2, -1) == -1) {
            perror("poll failed");
            exit(EXIT_FAILURE);
        }

        if (fds[0].revents & POLLIN) {
            update_input_buffer();
            if (has_whole_line()) {
                memset(buffer, 0, BUFFER_SIZE + 1);
                if (read_from_input(buffer, BUFFER_SIZE) > 0) {
                    buffer[strlen(buffer)] = '\0';
                    send(sockfd, buffer, strlen(buffer) + 1, 0);
                    if (strncmp(buffer, "bye", 3) == 0) {
                        endSession = 1;
                    }
                }
            }
        }

        if (fds[1].revents & POLLIN) {
            memset(buffer, 0, BUFFER_SIZE + 1);
            ssize_t bytesRead = recv(sockfd, buffer, BUFFER_SIZE, 0);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                write_to_output(buffer, strlen(buffer));
            } else if (bytesRead == 0) {
                fprint_to_output("Connection closed by peer. ^C to terminate.\n");
                endSession = 1;
            }
        }
    }

    if (!disableWindowing) {
        stop_windowing();
    }
}

int main(int argc, char *argv[]) {
    char *hostname = NULL;
    int port;

    parseCommandLine(argc, argv, &hostname, &port);
    if (port == -1) {
        fprintf(stderr, "Error: Port number is required.\n");
        exit(EXIT_FAILURE);
    }

    if (hostname) {
        if (verbosity > 0) {
            printf("Running in client mode. Connecting to %s:%d\n", hostname, port);
        }
        runClient(hostname, port);
    } else {
        if (verbosity > 0) {
            printf("Running in server mode. Listening on port %d\n", port);
        }
        runServer(port);
    }

    if (!disableWindowing) {
        stop_windowing();
    }

    return 0;
}
