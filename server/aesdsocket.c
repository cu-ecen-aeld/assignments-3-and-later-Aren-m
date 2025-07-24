#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/wait.h>
#include <netdb.h>
#include <signal.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define PORT "9000"
#define BACKLOG 10

#if USE_AESD_CHAR_DEVICE
#define FILE_PATH "/dev/aesdchar"
#else
#define FILE_PATH "/var/tmp/aesdsocketdata"
#endif

#define SLIST_FOREACH_SAFE(var, head, field, tvar) \
        for ((var) = SLIST_FIRST((head)); \
            (var) && ((tvar) = SLIST_NEXT((var), field), 1); \
            (var) = (tvar))


volatile sig_atomic_t end_signal_caught = false;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// thread args data struct
struct thread_data
{
    char s[INET6_ADDRSTRLEN];
    int threadfd;
};

// linked list of threads
struct slist_data_s
{
    pthread_t* thread_id;
    struct thread_data* args;
    SLIST_ENTRY(slist_data_s) entries;
};

// signal handling: SIGINT = ctrl + c, SIGTERM = process term (kill())
static void signal_handler(int signal_number)
{
    if ((signal_number == SIGINT) || (signal_number == SIGTERM))
    {
        end_signal_caught = true;
        syslog(LOG_INFO, "Caught signal, exiting");
    }       
}

// start as daemon
static void make_daemon()
{
    int pid = fork();
    if (pid == 0)
    {
        printf("daemon process started\n");
        if (setsid() == -1)
        {
            printf("daemon setid failure\n");
            exit(1);
        }
    }
    else
        exit( ( pid > 0 ) ? 0 : 1 ); 
}

void* fill_file(void* args)
{
    struct thread_data* thread_func_args = (struct thread_data *) args;
    int thread_server_fd = (*thread_func_args).threadfd;
    char threadbuf[512] = {0};
    char *thread_client_address = (*thread_func_args).s;

    int threadfiled = 0;
    int threadreadlen = 0;
    int threadwritestatus = 0;

    if ((threadfiled = open(FILE_PATH, O_RDWR | O_APPEND | O_CREAT, 0666)) == -1)
    {
        close((*thread_func_args).threadfd);
        printf("open file socket thread\n");
        exit(1);
    }
    
    pthread_mutex_lock(&mutex);
    while ((threadreadlen = recv(thread_server_fd, threadbuf, sizeof(threadbuf), 0)) > 0)
    {
        if ((threadwritestatus = write(threadfiled, threadbuf, threadreadlen)) == -1)
        {
            close(thread_server_fd);
            printf("write to file\n");
            pthread_mutex_unlock(&mutex); 
            exit(1);
        }
        else if (memchr(threadbuf, '\n', threadreadlen) != NULL)
        {
            if (lseek(threadfiled, 0, SEEK_SET) == -1)
            {
                printf("reset file position\n");
                pthread_mutex_unlock(&mutex); 
                exit(1);
            }

            while ((threadreadlen = read(threadfiled, threadbuf, sizeof(threadbuf))) > 0)
            {
                send(thread_server_fd, threadbuf, threadreadlen, 0);
            }

            pthread_mutex_unlock(&mutex); 
            close(threadfiled);
            close(thread_server_fd);
            syslog(LOG_INFO, "Closed connection from %s", thread_client_address);
            break;
        }
    }   

    return NULL;
}

void* append_timestamp(void* timeargs)
{
    int timerfiled = 0;
    int timerwritestatus = 0;

    while (!end_signal_caught)
    {
        sleep(10);

        pthread_mutex_lock(&mutex);
        time_t curtime = time(NULL);
        char timestamp_str[50] = {0};
        int len = strftime(timestamp_str, sizeof(timestamp_str), "timestamp: %Y %b %d %H:%M:%S\n", localtime(&curtime));

        if (len == 0)
        {
            pthread_mutex_unlock(&mutex);
            exit(1);
        }

        if(end_signal_caught)
        {
            pthread_mutex_unlock(&mutex);
            exit(1);
        }

        if ((timerfiled = open(FILE_PATH, O_RDWR | O_APPEND | O_CREAT, 0666)) == -1)
        {
            printf("open file timer thread\n");
            pthread_mutex_unlock(&mutex);
            exit(1);
        }
        if ((timerwritestatus = write(timerfiled, timestamp_str, len)) == -1)
        {
            close(timerfiled);
            printf("write timestamp\n");
            exit(1);
        }

        close(timerfiled);
        pthread_mutex_unlock(&mutex);
    }
    
    return NULL;
}

// to get IPv4 or IPv6 address from client
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &(((struct sockaddr_in*)sa)->sin_addr);
    
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

    SLIST_HEAD(slisthead, slist_data_s) head;
    SLIST_INIT(&head);
    struct slist_data_s *datap, *datap_temp;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((addrstatus = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0)
    {
        printf("getaddrinfo\n");
        exit(-1);
    }

    // servinfo points to linked list of struct of addrinfos, loop through and bind to first available
    for (p = servinfo; p != NULL; p = p->ai_next)
    {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
        {
            printf("open socket\n");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yep, sizeof(int)) == -1)
        {
            printf("setsockopt\n");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sockfd);
            printf("bind socket\n");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL)
    {
        printf("servinfo null\n");
        return -1;
    }

    if (listen(sockfd, BACKLOG) == -1)
    {
        close(sockfd);
        printf("listen on socket\n");
        exit(-1);
    }

    // fork after ensuring can bind on port
    if (argc == 2)
    {
        if (strcmp(argv[1], "-d") == 0)
            make_daemon();       
    }

#if !USE_AESD_CHAR_DEVICE
    // init timer thread
    pthread_t *timer_thread = malloc(sizeof(pthread_t));
    pthread_create(timer_thread, NULL, append_timestamp, NULL);
#endif

    // loop the process here until receive sigint or sigterm, then gracefully exit closing connections and deleting output file
    while (!end_signal_caught)
    {
        struct sockaddr* client_sock_addr = (struct sockaddr*)&client_addr;
        if ((newfd = accept(sockfd, client_sock_addr, &sin_size)) == -1)
        {
            printf("accept socket\n");
            continue;
        }

        inet_ntop(client_addr.ss_family, get_in_addr(client_sock_addr), s, sizeof(s));
        syslog(LOG_INFO, "Accepted connection from %s", s);

        struct thread_data* arg_data = malloc(sizeof(struct thread_data));
        strncpy(arg_data->s, s, INET6_ADDRSTRLEN);
        arg_data->threadfd = newfd;
        
        datap = malloc(sizeof(struct slist_data_s));
        datap->thread_id = malloc(sizeof(pthread_t));
        datap->args = arg_data;
        SLIST_INSERT_HEAD(&head, datap, entries);

        pthread_create(datap->thread_id, NULL, fill_file, arg_data);
    }
    
    close(sockfd);

    SLIST_FOREACH_SAFE(datap, &head, entries, datap_temp)
    {
        pthread_join(*(datap->thread_id), NULL);
        free(datap->args);
        free(datap->thread_id);
        SLIST_REMOVE(&head, datap, slist_data_s, entries);
        free(datap);
    }

#if !USE_AESD_CHAR_DEVICE
    pthread_cancel(*timer_thread);
    pthread_join(*timer_thread, NULL);
    free(timer_thread);
    remove(FILE_PATH);
#endif

    pthread_mutex_destroy(&mutex);
    return 0;
}