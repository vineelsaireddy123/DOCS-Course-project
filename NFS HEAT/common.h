#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

#define MAX_BUFFER 8192
#define MAX_PATH 512
#define MAX_FILENAME 256
#define MAX_USERNAME 64
#define MAX_FILES 10000
#define MAX_CLIENTS 100
#define MAX_SS 50
#define MAX_SENTENCE_LEN 4096
#define MAX_WORDS 1024
#define MAX_CHECKPOINTS 50
#define MAX_CHECKPOINT_TAG 128

// Error codes
#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 1
#define ERR_UNAUTHORIZED 2
#define ERR_FILE_EXISTS 3
#define ERR_SENTENCE_LOCKED 4
#define ERR_INVALID_INDEX 5
#define ERR_SS_UNAVAILABLE 6
#define ERR_INVALID_COMMAND 7
#define ERR_PERMISSION_DENIED 8

// Message types
#define MSG_REGISTER_SS 100
#define MSG_REGISTER_CLIENT 101
#define MSG_CREATE_FILE 102
#define MSG_DELETE_FILE 103
#define MSG_READ_FILE 104
#define MSG_WRITE_FILE 105
#define MSG_INFO_FILE 106
#define MSG_LIST_FILES 107
#define MSG_STREAM_FILE 108
#define MSG_EXEC_FILE 109
#define MSG_LIST_USERS 110
#define MSG_ADD_ACCESS 111
#define MSG_REM_ACCESS 112
#define MSG_UNDO 113
#define MSG_GET_OWNER 114
#define MSG_CREATE_FOLDER 115
#define MSG_MOVE_FILE 116
#define MSG_VIEW_FOLDER 117
#define MSG_CHECKPOINT 118
#define MSG_VIEWCHECKPOINT 119
#define MSG_REVERT 120
#define MSG_LISTCHECKPOINTS 121
#define MSG_RESPONSE 200
#define MSG_ERROR 201
#define MSG_ACK 202

// Access levels
#define ACCESS_NONE 0
#define ACCESS_READ 1
#define ACCESS_WRITE 2

// Structures
typedef struct {
    char folder_path[MAX_PATH];
    char owner[MAX_USERNAME];
    time_t created;
} FolderInfo;

typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    time_t created;
    time_t modified;
    time_t accessed;
    long size;
    int word_count;
    int char_count;
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    char folder_path[MAX_PATH]; // Path to the folder containing this file
} FileInfo;

typedef struct {
    char username[MAX_USERNAME];
    int access_level; // ACCESS_READ or ACCESS_WRITE
} AccessEntry;

typedef struct {
    char tag[MAX_CHECKPOINT_TAG];
    char content[MAX_BUFFER * 4];
    time_t timestamp;
    char username[MAX_USERNAME];
} Checkpoint;

typedef struct {
    char filename[MAX_FILENAME];
    AccessEntry entries[MAX_CLIENTS];
    int num_entries;
} AccessControl;

typedef struct {
    int type;
    char username[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char data[MAX_BUFFER];
    int sentence_num;
    int word_index;
    int error_code;
    int flags;
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    char folder_path[MAX_PATH]; // For folder operations
} Message;

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int nm_port;
    int client_port;
    char files[MAX_FILES][MAX_FILENAME];
    int num_files;
    int active;
    pthread_mutex_t lock;
} StorageServerInfo;

typedef struct {
    char username[MAX_USERNAME];
    char ip[INET_ADDRSTRLEN];
    int port;
    int sockfd;
    int active;
} ClientInfo;

// Function declarations
void log_message(const char *component, const char *message);
void log_request(const char *component, const char *ip, int port, const char *request);
char *get_timestamp();
int send_message(int sockfd, Message *msg);
int receive_message(int sockfd, Message *msg);
void init_message(Message *msg);

#endif