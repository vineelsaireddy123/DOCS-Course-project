#include "common.h"

void log_message(const char *component, const char *message) {
    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    FILE *logfile = fopen("system.log", "a");
    if (logfile) {
        fprintf(logfile, "[%s] [%s] %s\n", timestamp, component, message);
        fclose(logfile);
    }
    printf("[%s] [%s] %s\n", timestamp, component, message);
}

void log_request(const char *component, const char *ip, int port, const char *request) {
    char log_buf[MAX_BUFFER];
    snprintf(log_buf, sizeof(log_buf), "Request from %s:%d - %s", ip, port, request);
    log_message(component, log_buf);
}

char *get_timestamp() {
    static char buffer[64];
    time_t now = time(NULL);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", localtime(&now));
    return buffer;
}

void init_message(Message *msg) {
    memset(msg, 0, sizeof(Message));
}

int send_message(int sockfd, Message *msg) {
    int total_sent = 0;
    int bytes_left = sizeof(Message);
    int n;
    
    while (total_sent < sizeof(Message)) {
        n = send(sockfd, ((char*)msg) + total_sent, bytes_left, 0);
        if (n <= 0) {
            return -1;
        }
        total_sent += n;
        bytes_left -= n;
    }
    return 0;
}

int receive_message(int sockfd, Message *msg) {
    int total_received = 0;
    int bytes_left = sizeof(Message);
    int n;
    
    while (total_received < sizeof(Message)) {
        n = recv(sockfd, ((char*)msg) + total_received, bytes_left, 0);
        if (n <= 0) {
            return -1;
        }
        total_received += n;
        bytes_left -= n;
    }
    return 0;
}