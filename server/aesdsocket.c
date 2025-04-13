#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/wait.h>
#include <signal.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>


#define PORT "9000"
#define BACKLOG 10

volatile sig_atomic_t end_signal_caught = false;

// signal handling
static void signal_handler(int signal_number)
{
    if((signal_number == SIGINT) || (signal_number == SIGTERM))
    {
        remove("/var/tmp/aesdsocketdata");
        end_signal_caught = true;
        syslog(LOG_INFO, "Caught signal, exiting");
    }       
}

// start as daemon
static void make_daemon()
{
    int pid = fork();

    if(pid < 0)
    {
        printf("fork\n");
        exit(1);
    }
        
    if(pid > 0)
    {
        exit(0);
    }

    if(pid == 0)
    {
        printf("daemon process started\n");
        if(setsid() == -1)
        {
            printf("daemon setid failure\n");
            exit(1);
        }
    }
}

// to get IPv4 or IPv6 address from client
void *get_in_addr(struct sockaddr *sa)
{
    if(sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


int main(int argc, char *argv[])
{
    struct sigaction end_action;
    memset(&end_action, 0, sizeof(struct sigaction));
    end_action.sa_handler = signal_handler;
    sigaction(SIGTERM, &end_action, NULL);
    sigaction(SIGINT, &end_action, NULL);

    int addrstatus = 0;
    int sockfd = 0;
    int newfd = 0;
    int yep = 1;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage client_addr;
    socklen_t sin_size = sizeof(client_addr);
    char s[INET6_ADDRSTRLEN];

    int filed = 0;
    int readlen = 0;
    int writestatus = 0;
    char msgbuf[512] = {0};

    memset(&hints, 0, sizeof hints);
    hints.ai_family = PF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if((addrstatus = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0)
    {
        printf("getaddrinfo\n");
        exit(-1);
    }

    // servinfo points to linked list of struct of addrinfos, loop through and bind to first available
    for(p = servinfo; p != NULL; p = p->ai_next)
    {
        if((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
        {
            printf("open socket\n");
            continue;
        }

        if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yep, sizeof(int)) == -1)
        {
            printf("setsockopt\n");
            exit(1);
        }

        if(bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sockfd);
            printf("bind socket\n");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if(p == NULL)
    {
        printf("failed to bind\n");
        return -1;
    }

    if(listen(sockfd, BACKLOG) == -1)
    {
        close(sockfd);
        printf("listen on socket\n");
        exit(-1);
    }

    // fork after ensuring can bind on port
    if(argc == 2)
    {
        if(strcmp(argv[1], "-d") == 0)
        {
            make_daemon();
        }       
    }

    // loop the process here until receive sigint or sigterm, then gracefully exit closing connections and deleting output file
    while(!end_signal_caught)
    {
        if((newfd = accept(sockfd, (struct sockaddr *)&client_addr, &sin_size)) == -1)
        {
            printf("accept socket\n");
            exit(-1);
        }

        inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr *)&client_addr), s, sizeof(s));

        syslog(LOG_INFO, "Accepted connection from %s", s);
    
        // read and return full data back to client when completed datapacket
        if((filed = open("/var/tmp/aesdsocketdata", O_RDWR | O_APPEND | O_CREAT, 0666)) == -1)
        {
            close(newfd);
            printf("open file\n");
            exit(1);
        }

        while((readlen = recv(newfd, msgbuf, sizeof(msgbuf), 0)) > 0)
        {
            if((writestatus = write(filed, msgbuf, readlen)) == -1)
            {
                close(newfd);
                printf("write to file\n");
                exit(1);
            }
            else if(memchr(msgbuf, '\n', readlen) != NULL)
            {
                //  send to client from file
                if(lseek(filed, 0, SEEK_SET) == -1)
                {
                    printf("reset file descriptor\n");
                    exit(1);
                }

                while((readlen = read(filed, msgbuf, sizeof(msgbuf))) > 0)
                {
                    send(newfd, msgbuf, readlen, 0);
                }
            }
        }

        close(filed);
        close(newfd);
        syslog(LOG_INFO, "Closed connection from %s", s);
    }
    
    close(sockfd);

    return 0;
}