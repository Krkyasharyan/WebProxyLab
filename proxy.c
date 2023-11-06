/*
 * proxy.c - ICS Web proxy
 *
 *
 */

#include "csapp.h"
#include <stdarg.h>
#include <sys/select.h>

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, char *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, size_t size);
char* extract_uri(char* request);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
ssize_t Rio_writen_w(int fd, void *usrbuf, size_t n);
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n);

/*
 * The pthread_create() function in the C programming 
 * language requires a function pointer with this specific 
 * signature: void *(*start_routine) (void *). 
 * This means that the function must take a single void args structure
 */
typedef struct {
    int clientSocketFD;
    struct sockaddr_in clientAddress;
} ThreadArgs;

void *proxyThread(void *threadArgs)
{
    pthread_detach(pthread_self());
    int clientSocketFD = ((ThreadArgs *)threadArgs)->clientSocketFD;
    struct sockaddr_in *clientAddress = &((ThreadArgs *)threadArgs)->clientAddress;
    processClientRequest(clientSocketFD, clientAddress);
    Free(threadArgs);
    return NULL;
}

// Semaphores for concurrency control
sem_t serverConnectSem, logSem, reqSem;

/*
 * main - Main routine for the proxy program
 */

int main(int argc, char **argv)
{
   /* Check arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        exit(0);
    }

    // Ignore SIGPIPE signals to prevent program termination
    Signal(SIGPIPE, SIG_IGN);

    // Initialize semaphores
    Sem_init(&serverConnectSem, 0, 1);
    Sem_init(&logSem, 0, 1);
    Sem_init(&reqSem, 0, 1);

    char *clientPort = argv[1];

    // Open and return a listening socket on port.
    int listenFD = Open_listenfd(clientPort);

    // Accept incoming connection requests from clients.
    struct sockaddr_storage clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    while(1){

        // Accept a new connection from the client.
        int clientFD = Accept(listenFD, (struct sockaddr*)&clientAddr, &clientAddrLen);

        // Create a new thread to handle the request.
        pthread_t tid;
        ThreadArgs *args = Malloc(sizeof(ThreadArgs));
        args->clientSocketFD = clientFD;
        args->clientAddress = *((struct sockaddr_in*)&clientAddr);
        pthread_create(&tid, NULL, proxyThread, args); // We don't use wrapper function because we don't want to exit on error
    }

    exit(0);
}

// Parse the client request and send it to the server
int parseAndSendRequest(int clientSocketFD, rio_t *clientRio, int *requestLen)
{
    char buf[MAXLINE];
    int contentLen, bytesRead, count;

    contentLen = 0;
    count = 0;
    if ((bytesRead = Rio_readlineb_w(clientRio, buf, MAXLINE)) <= 0) return -1;
    while (strcmp(buf, "\r\n")) {
        count += bytesRead;
        if (strcasestr(buf, "Content-Length: ")) {
            sscanf(buf + strlen("Content-Length: "), "%d", &contentLen);
        }
        if (buf[strlen(buf) - 1] != '\n') return -1;
        if (Rio_writen_w(clientSocketFD, buf, strlen(buf)) <= 0) return -1;
        if ((bytesRead = Rio_readlineb_w(clientRio, buf, MAXLINE)) <= 0) return -1;
    }
    count += bytesRead;
    if (Rio_writen_w(clientSocketFD, buf, strlen(buf)) <= 0) return -1;
    if (requestLen != NULL) {
        *requestLen += count;
    }
    return contentLen;
}

/* --- Sub-functions --- */

void closeConnections(int clientSocketFD, int serverSocketFD) {
    Close(clientSocketFD);
    Close(serverSocketFD);
}

void handleError(int clientSocketFD, int serverSocketFD, const char* errorMessage) {
    fprintf(stderr, "%s\n", errorMessage);
    closeConnections(clientSocketFD, serverSocketFD);
}

void sendRequest(int serverSocketFD, char* request) {
    if (Rio_writen_w(serverSocketFD, request, strlen(request)) <= 0) {
        handleError(serverSocketFD, serverSocketFD, "rio_writen error");
        return;
    }
}

void receiveAndSendHeader(int clientSocketFD, int serverSocketFD, rio_t *clientRio, int *contentLen) {
    *contentLen = parseAndSendRequest(serverSocketFD, clientRio, NULL);
    if (*contentLen < 0) {
        handleError(clientSocketFD, serverSocketFD, "parseAndSendRequest error");
        return;
    }
}

void handlePOSTRequest(int clientSocketFD, int serverSocketFD, rio_t *clientRio, int contentLen, char* body, char* method) {
    int bytesRead;
    if (strcasecmp(method, "POST") == 0) {
        while (contentLen > 0) {
            bytesRead = Rio_readnb_w(clientRio, body, contentLen > 102400 ? 102400 : contentLen);
            if (bytesRead <= 0) {
                handleError(clientSocketFD, serverSocketFD, "handlePOSTRequest read body error");
                return;
            }
            if (Rio_writen_w(serverSocketFD, body, bytesRead) <= 0) {
                handleError(clientSocketFD, serverSocketFD, "handlePOSTRequest write body error");
                return;
            }
            contentLen -= bytesRead;
        }
    }
}

void receiveBody(int clientSocketFD, int serverSocketFD, rio_t *clientRio, int *contentLen, int *totalCount, char* body) {
    int bytesRead;
    *totalCount += *contentLen;
    while (*contentLen > 0) {
        bytesRead = Rio_readnb_w(clientRio, body, 1);
        if (bytesRead <= 0) {
            handleError(clientSocketFD, serverSocketFD, "receiveBody read body error");
            return;
        }
        if (Rio_writen_w(clientSocketFD, body, 1) <= 0) {
            handleError(clientSocketFD, serverSocketFD, "receiveBody write body error");
            return;
        }
        (*contentLen)--;
    }
}

void logRequest(struct sockaddr_in *clientAddress, char* uri, int totalCount, char* logEntry) {
    if (totalCount > 0) {
        format_log_entry(logEntry, clientAddress, uri, totalCount);
        P(&logSem);
        printf("%s\n", logEntry);
        fflush(stdout);
        V(&logSem);
    }
}

/* --- Main function --- */

void processClientRequest(int clientSocketFD, struct sockaddr_in *clientAddress)
{
    int serverSocketFD;
    int  totalCount, contentLen;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], pathname[MAXLINE], port[MAXLINE];
    char request[MAXLINE];
    char body[102400];
    char logEntry[MAXLINE];
    rio_t clientRio;

    rio_readinitb(&clientRio, clientSocketFD);

    /* Parse request header */
    if (Rio_readlineb_w(&clientRio, buf, MAXLINE) <= 0) {
        handleError(clientSocketFD, clientSocketFD, "Read request header error");
        return;
    }

    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
        handleError(clientSocketFD, clientSocketFD, "SScanf parse request error");
        return;
    }

    if (parse_uri(uri, hostname, pathname, port) < 0) {
        handleError(clientSocketFD, clientSocketFD, "Parse uri error");
        return;
    }

    /* Connect to server */
    P(&serverConnectSem);
    if ((serverSocketFD = open_clientfd(hostname, port)) < 0) {
        V(&serverConnectSem);
        handleError(clientSocketFD, clientSocketFD, "Open clientfd error");
        return;
    }
    V(&serverConnectSem);

    if (pathname[0] == '\0') {
        pathname[0] = '/';
        pathname[1] = '\0';
    }

    P(&reqSem);
    sprintf(request, "%s %s %s\r\n", method, pathname, version);
    V(&reqSem);

    /* Send request to server */
    sendRequest(serverSocketFD, request);

    /* Read and send header */
    receiveAndSendHeader(clientSocketFD, serverSocketFD, &clientRio, &contentLen);

    /* If it's a POST request, send body */
    handlePOSTRequest(clientSocketFD, serverSocketFD, &clientRio, contentLen, body, method);

    rio_readinitb(&clientRio, serverSocketFD);

    /* Receive response from server and forward to client */
    totalCount = 0;
    contentLen = parseAndSendRequest(clientSocketFD, &clientRio, &totalCount);
    if (contentLen < 0) {
        handleError(clientSocketFD, serverSocketFD, "parseAndSendRequest error");
        return;
    }

    /* Receive body */
    receiveBody(clientSocketFD, serverSocketFD, &clientRio, &contentLen, &totalCount, body);

    closeConnections(clientSocketFD, serverSocketFD);

    /* Log the request */
    logRequest(clientAddress, uri, totalCount, logEntry);
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, char *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
    }


    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    if (hostend == NULL)
        return -1;
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    /* Extract the port number */
    if (*hostend == ':') {
        char *p = hostend + 1;
        while (isdigit(*p))
            *port++ = *p++;
        *port = '\0';
    } else {
        strcpy(port, "80");
    }

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathname[0] = '\0';
    }
    else {
        strcpy(pathname, pathbegin);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), the number of bytes
 * from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr,
                      char *uri, size_t size)
{
    time_t now;
    char time_str[MAXLINE];
    char host[INET_ADDRSTRLEN];

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    if (inet_ntop(AF_INET, &sockaddr->sin_addr, host, sizeof(host)) == NULL)
        unix_error("Convert sockaddr_in to string representation failed\n");

    /* Return the formatted log entry string */
    P(&logSem);
    sprintf(logstring, "%s: %s %s %zu", time_str, host, uri, size);
    V(&logSem);
}


// Rio wrapper functions to prevent exiting on error
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen){
    ssize_t rc;
    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0){
        printf("rio_readlineb error\n");
        return 0;
    }
    return rc;
}

ssize_t Rio_writen_w(int fd, void *usrbuf, size_t n){
    ssize_t rc;
    if ((rc = rio_writen(fd, usrbuf, n)) != n){
        printf("rio_writen error\n");
        return 0;
    }
    return rc;
}

ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n){
    ssize_t rc;
    if ((rc = rio_readnb(rp, usrbuf, n)) < 0){
        printf("rio_readnb error\n");
        return 0;
    }
    return rc;
}