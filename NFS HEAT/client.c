#include "common.h"

char username[MAX_USERNAME];
char nm_ip[INET_ADDRSTRLEN];
int nm_port;

int connect_to_nm() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(nm_port);
    inet_pton(AF_INET, nm_ip, &addr.sin_addr);
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Failed to connect to Naming Server");
        return -1;
    }
    return sockfd;
}

void handle_view(int flags) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_LIST_FILES;
    strcpy(msg.username, username);
    msg.flags = (flags & 1); // -a flag
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_RESPONSE) {
        if (flags & 2) { // -l flag
            printf("---------------------------------------------------------\n");
            printf("|  Filename  | Words | Chars | Last Access Time | Owner |\n");
            printf("|------------|-------|-------|------------------|-------|\n");
            
            char temp_data[MAX_BUFFER];
            strcpy(temp_data, response.data);
            char *line = strtok(temp_data, "\n");
            while (line) {
                // Get file owner from naming server
                int nm_sock_owner = connect_to_nm();
                char owner[MAX_USERNAME] = "unknown";
                
                if (nm_sock_owner >= 0) {
                    Message owner_msg;
                    init_message(&owner_msg);
                    owner_msg.type = MSG_GET_OWNER;
                    strcpy(owner_msg.filename, line);
                    send_message(nm_sock_owner, &owner_msg);
                    
                    Message owner_resp;
                    if (receive_message(nm_sock_owner, &owner_resp) == 0 && 
                        owner_resp.type == MSG_RESPONSE && strlen(owner_resp.data) > 0) {
                        strcpy(owner, owner_resp.data);
                    }
                    close(nm_sock_owner);
                }
                
                // Get file info from storage server
                int nm_sock2 = connect_to_nm();
                int word_count = 0;
                int char_count = 0;
                char last_access[20] = "N/A";
                
                if (nm_sock2 >= 0) {
                    Message info_msg;
                    init_message(&info_msg);
                    info_msg.type = MSG_READ_FILE;
                    strcpy(info_msg.filename, line);
                    strcpy(info_msg.username, username);
                    
                    send_message(nm_sock2, &info_msg);
                    
                    Message info_resp;
                    receive_message(nm_sock2, &info_resp);
                    close(nm_sock2);
                    
                    if (info_resp.type == MSG_RESPONSE) {
                        // Connect to SS to get file info
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(info_resp.ss_port);
                        inet_pton(AF_INET, info_resp.ss_ip, &ss_addr.sin_addr);
                        
                        if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                            init_message(&info_msg);
                            info_msg.type = MSG_INFO_FILE;
                            strcpy(info_msg.filename, line);
                            send_message(ss_sock, &info_msg);
                            
                            Message file_info;
                            if (receive_message(ss_sock, &file_info) == 0 && file_info.type == MSG_RESPONSE) {
                                // Parse the info response to get word count, char count, and last modified time
                                char *words_str = strstr(file_info.data, "Words: ");
                                char *chars_str = strstr(file_info.data, "Chars: ");
                                char *modified_str = strstr(file_info.data, "Modified: ");
                                
                                if (words_str) {
                                    sscanf(words_str, "Words: %d", &word_count);
                                }
                                if (chars_str) {
                                    sscanf(chars_str, "Chars: %d", &char_count);
                                }
                                if (modified_str) {
                                    // Extract timestamp - format from ctime: "Dow Mon DD HH:MM:SS YYYY\n"
                                    // Example: "Tue Nov 19 01:23:45 2024\n"
                                    char dow[4], mon[4], day[3], time_str[9], year[5];
                                    if (sscanf(modified_str, "Modified: %3s %3s %2s %8s %4s", 
                                               dow, mon, day, time_str, year) == 5) {
                                        // Convert month name to number
                                        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
                                        int mon_num = 1;
                                        for (int i = 0; i < 12; i++) {
                                            if (strcmp(mon, months[i]) == 0) {
                                                mon_num = i + 1;
                                                break;
                                            }
                                        }
                                        
                                        // Extract hour and minute
                                        char hour[3], min[3];
                                        sscanf(time_str, "%2s:%2s", hour, min);
                                        
                                        // Format as "YYYY-MM-DD HH:MM"
                                        snprintf(last_access, sizeof(last_access), "%s-%02d-%s %s:%s", 
                                                year, mon_num, day, hour, min);
                                    }
                                }
                            }
                            close(ss_sock);
                        }
                    }
                }
                
                printf("| %-10s | %-5d | %-5d | %-16s | %-5s |\n",
                       line, word_count, char_count, last_access, owner);
                line = strtok(NULL, "\n");
            }
            printf("---------------------------------------------------------\n");
        } else {
            printf("Files:\n");
            char temp_data[MAX_BUFFER];
            strcpy(temp_data, response.data);
            char *line = strtok(temp_data, "\n");
            while (line) {
                printf("--> %s\n", line);
                line = strtok(NULL, "\n");
            }
        }
    }
}

void handle_read(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_READ_FILE;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    
    if (response.type == MSG_ERROR) {
        printf("Error: ");
        switch (response.error_code) {
            case ERR_FILE_NOT_FOUND:
                printf("File not found\n");
                break;
            case ERR_UNAUTHORIZED:
                printf("Access denied\n");
                break;
            default:
                printf("Unknown error\n");
        }
        close(nm_sock);
        return;
    }
    
    // Connect to SS
    close(nm_sock);
    
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(response.ss_port);
    inet_pton(AF_INET, response.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("Error: Storage Server unavailable\n");
        return;
    }
    
    init_message(&msg);
    msg.type = MSG_READ_FILE;
    strcpy(msg.filename, filename);
    send_message(ss_sock, &msg);
    
    receive_message(ss_sock, &response);
    close(ss_sock);
    
    if (response.type == MSG_RESPONSE) {
        printf("%s\n", response.data);
    }
}

void handle_create(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_CREATE_FILE;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("File Created Successfully!\n");
    } else {
        printf("Error: File creation failed\n");
    }
}

void handle_write(const char *filename, int sentence_num) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_WRITE_FILE;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    msg.sentence_num = sentence_num;
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    
    if (response.type == MSG_ERROR) {
        printf("Error: ");
        switch (response.error_code) {
            case ERR_FILE_NOT_FOUND:
                printf("File not found\n");
                break;
            case ERR_UNAUTHORIZED:
                printf("Access denied\n");
                break;
            case ERR_SENTENCE_LOCKED:
                printf("File is currently being accessed by another user\n");
                break;
            default:
                printf("%s\n", response.data);
        }
        close(nm_sock);
        return;
    }
    
    close(nm_sock);
    
    // Connect to SS and acquire lock FIRST
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(response.ss_port);
    inet_pton(AF_INET, response.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("Error: Storage Server unavailable\n");
        return;
    }
    
    // Send initial WRITE request to acquire lock
    init_message(&msg);
    msg.type = MSG_WRITE_FILE;
    strcpy(msg.filename, filename);
    msg.sentence_num = sentence_num;
    strcpy(msg.data, ""); // Empty data to signal lock acquisition
    
    send_message(ss_sock, &msg);
    
    // Wait for lock acknowledgment
    Message lock_response;
    receive_message(ss_sock, &lock_response);
    
    if (lock_response.type == MSG_ERROR) {
        printf("Error: %s\n", lock_response.data);
        close(ss_sock);
        return;
    }
    
    // Lock acquired! Now prompt user for commands
    char write_data[MAX_BUFFER] = "";
    char line[MAX_SENTENCE_LEN];
    
    printf("Enter write commands (end with ETIRW):\n");
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = 0;
        
        if (strcmp(line, "ETIRW") == 0) {
            strcat(write_data, "ETIRW\n");
            break;
        }
        
        strcat(write_data, line);
        strcat(write_data, "\n");
    }
    
    // Send the actual write data
    init_message(&msg);
    msg.type = MSG_WRITE_FILE;
    strcpy(msg.filename, filename);
    msg.sentence_num = sentence_num;
    strcpy(msg.data, write_data);
    
    send_message(ss_sock, &msg);
    receive_message(ss_sock, &response);
    close(ss_sock);
    
    if (response.type == MSG_ACK) {
        printf("Write Successful!\n");
    } else {
        printf("Error: Write failed - %s\n", response.data);
    }
}

void handle_delete(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_DELETE_FILE;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("File '%s' deleted successfully!\n", filename);
    } else {
        printf("Error: File deletion failed\n");
    }
}

void handle_stream(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_STREAM_FILE;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    
    if (response.type == MSG_ERROR) {
        printf("Error: Cannot stream file\n");
        close(nm_sock);
        return;
    }
    
    close(nm_sock);
    
    // Connect to SS
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(response.ss_port);
    inet_pton(AF_INET, response.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("Error: Storage Server unavailable\n");
        return;
    }
    
    init_message(&msg);
    msg.type = MSG_STREAM_FILE;
    strcpy(msg.filename, filename);
    send_message(ss_sock, &msg);
    
    // Receive and display words
    while (1) {
        if (receive_message(ss_sock, &response) < 0) {
            printf("\nError: Storage Server disconnected\n");
            break;
        }
        
        if (response.type == MSG_ACK && strcmp(response.data, "STOP") == 0) {
            printf("\n");
            break;
        }
        
        printf("%s ", response.data);
        fflush(stdout);
    }
    
    close(ss_sock);
}

void handle_info(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_READ_FILE; // Get SS info first
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    
    if (response.type == MSG_ERROR) {
        printf("Error: Cannot get file info\n");
        close(nm_sock);
        return;
    }
    
    close(nm_sock);
    
    // Connect to SS for file info
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(response.ss_port);
    inet_pton(AF_INET, response.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("Error: Storage Server unavailable\n");
        return;
    }
    
    init_message(&msg);
    msg.type = MSG_INFO_FILE;
    strcpy(msg.filename, filename);
    send_message(ss_sock, &msg);
    
    receive_message(ss_sock, &response);
    close(ss_sock);
    
    if (response.type == MSG_RESPONSE) {
        printf("--> File: %s\n", filename);
        printf("--> Owner: %s\n", username);
        printf("%s", response.data);
    } else {
        printf("Error: Cannot get file info\n");
    }
}

void handle_list_users() {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_LIST_USERS;
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_RESPONSE) {
        printf("Users:\n");
        char *line = strtok(response.data, "\n");
        while (line) {
            printf("--> %s\n", line);
            line = strtok(NULL, "\n");
        }
    }
}

void handle_undo(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_READ_FILE; // Get SS info first
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    
    if (response.type == MSG_ERROR) {
        printf("Error: Cannot access file\n");
        close(nm_sock);
        return;
    }
    
    close(nm_sock);
    
    // Connect to SS for undo
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(response.ss_port);
    inet_pton(AF_INET, response.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("Error: Storage Server unavailable\n");
        return;
    }
    
    init_message(&msg);
    msg.type = MSG_UNDO;
    strcpy(msg.filename, filename);
    send_message(ss_sock, &msg);
    
    receive_message(ss_sock, &response);
    close(ss_sock);
    
    if (response.type == MSG_ACK) {
        printf("Undo Successful!\n");
    } else {
        printf("Error: Undo failed\n");
    }
}

void handle_add_access(const char *flag, const char *filename, const char *target_user) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_ADD_ACCESS;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    strcpy(msg.data, target_user);
    msg.flags = (strcmp(flag, "-R") == 0) ? 1 : 2; // 1 for read, 2 for write
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("Access granted successfully!\n");
    } else {
        printf("Error: Failed to grant access\n");
    }
}

void handle_rem_access(const char *filename, const char *target_user) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_REM_ACCESS;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    strcpy(msg.data, target_user);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("Access removed successfully!\n");
    } else {
        printf("Error: Failed to remove access\n");
    }
}

void handle_exec(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_EXEC_FILE;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_RESPONSE) {
        printf("%s", response.data);
    } else {
        printf("Error: Cannot execute file\n");
    }
}

// ===== FOLDER MANAGEMENT HANDLERS =====

void handle_create_folder(const char *foldername) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_CREATE_FOLDER;
    strcpy(msg.folder_path, foldername);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("Folder '%s' created successfully!\n", foldername);
    } else {
        printf("Error: Failed to create folder\n");
    }
}

void handle_move_file(const char *filename, const char *foldername) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_MOVE_FILE;
    strcpy(msg.filename, filename);
    strcpy(msg.folder_path, foldername);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("File '%s' moved to folder '%s' successfully!\n", filename, foldername);
    } else {
        printf("Error: Failed to move file to folder\n");
    }
}

void handle_view_folder(const char *foldername) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_VIEW_FOLDER;
    strcpy(msg.folder_path, foldername);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_RESPONSE) {
        printf("\n=== Contents of folder '%s' ===\n", foldername);
        printf("%s", response.data);
        printf("===============================\n\n");
    } else {
        printf("Error: Failed to view folder contents\n");
    }
}

// ===== END FOLDER MANAGEMENT HANDLERS =====

// ===== CHECKPOINT MANAGEMENT HANDLERS =====

void handle_checkpoint(const char *filename, const char *tag) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_CHECKPOINT;
    strcpy(msg.filename, filename);
    strcpy(msg.data, tag);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("Checkpoint '%s' created successfully for file '%s'!\n", tag, filename);
    } else {
        printf("Error: %s\n", response.data);
    }
}

void handle_view_checkpoint(const char *filename, const char *tag) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_VIEWCHECKPOINT;
    strcpy(msg.filename, filename);
    strcpy(msg.data, tag);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_RESPONSE) {
        printf("\n=== Checkpoint '%s' of file '%s' ===\n", tag, filename);
        printf("%s\n", response.data);
        printf("====================================\n\n");
    } else {
        printf("Error: %s\n", response.data);
    }
}

void handle_revert_checkpoint(const char *filename, const char *tag) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_REVERT;
    strcpy(msg.filename, filename);
    strcpy(msg.data, tag);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_ACK) {
        printf("File '%s' reverted to checkpoint '%s' successfully!\n", filename, tag);
    } else {
        printf("Error: %s\n", response.data);
    }
}

void handle_list_checkpoints(const char *filename) {
    int nm_sock = connect_to_nm();
    if (nm_sock < 0) return;
    
    Message msg;
    init_message(&msg);
    msg.type = MSG_LISTCHECKPOINTS;
    strcpy(msg.filename, filename);
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    receive_message(nm_sock, &response);
    close(nm_sock);
    
    if (response.type == MSG_RESPONSE) {
        printf("\n=== Checkpoints for file '%s' ===\n", filename);
        if (strlen(response.data) == 0) {
            printf("No checkpoints found.\n");
        } else {
            printf("%s", response.data);
        }
        printf("==================================\n\n");
    } else {
        printf("Error: %s\n", response.data);
    }
}

// ===== END CHECKPOINT MANAGEMENT HANDLERS =====

void print_help() {
    printf("\nAvailable Commands:\n");
    printf("  VIEW [-a] [-l]              - List files\n");
    printf("  READ <filename>             - Read file content\n");
    printf("  CREATE <filename>           - Create new file\n");
    printf("  WRITE <filename> <sent#>    - Write to file\n");
    printf("  DELETE <filename>           - Delete file\n");
    printf("  STREAM <filename>           - Stream file content\n");
    printf("  INFO <filename>             - Get file information\n");
    printf("  LIST                        - List all users\n");
    printf("  UNDO <filename>             - Undo last change\n");
    printf("  ADDACCESS -R/-W <file> <user> - Add access\n");
    printf("  REMACCESS <file> <user>     - Remove access\n");
    printf("  EXEC <filename>             - Execute file\n");
    printf("  CREATEFOLDER <foldername>  - Create folder\n");
    printf("  MOVE <filename> <folder>    - Move file to folder\n");
    printf("  VIEWFOLDER <foldername>    - View folder contents\n");
    printf("  CHECKPOINT <file> <tag>    - Create checkpoint\n");
    printf("  VIEWCHECKPOINT <file> <tag>- View checkpoint\n");
    printf("  REVERT <file> <tag>        - Revert to checkpoint\n");
    printf("  LISTCHECKPOINTS <filename> - List all checkpoints\n");
    printf("  HELP                        - Show this help\n");
    printf("  EXIT                        - Exit client\n\n");
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <nm_ip> <nm_port>\n", argv[0]);
        return 1;
    }
    
    strcpy(nm_ip, argv[1]);
    nm_port = atoi(argv[2]);
    
    printf("Enter username: ");
    if (!fgets(username, sizeof(username), stdin)) {
        return 1;
    }
    username[strcspn(username, "\n")] = 0;
    
    // Register with naming server
    int nm_sock = connect_to_nm();
    if (nm_sock >= 0) {
        Message reg_msg;
        init_message(&reg_msg);
        reg_msg.type = MSG_REGISTER_CLIENT;
        strcpy(reg_msg.username, username);
        gethostname(reg_msg.ss_ip, sizeof(reg_msg.ss_ip));
        send_message(nm_sock, &reg_msg);
        
        Message ack;
        receive_message(nm_sock, &ack);
        close(nm_sock);
    }
    
    printf("Welcome, %s!\n", username);
    print_help();
    
    char command[MAX_BUFFER];
    while (1) {
        printf("%s> ", username);
        if (!fgets(command, sizeof(command), stdin)) break;
        command[strcspn(command, "\n")] = 0;
        
        char cmd_copy[MAX_BUFFER];
        strcpy(cmd_copy, command);
        char *cmd = strtok(cmd_copy, " ");
        if (!cmd) continue;
        
        if (strcmp(cmd, "EXIT") == 0) {
            break;
        } else if (strcmp(cmd, "HELP") == 0) {
            print_help();
        } else if (strcmp(cmd, "VIEW") == 0) {
            // Validate VIEW command - only allow VIEW, VIEW -a, VIEW -l, VIEW -al
            int flags = 0;
            char *flag = strtok(NULL, " ");
            int valid_flags = 1;
            
            if (flag == NULL) {
                // Just "VIEW" is valid
                flags = 0;
            } else if (strcmp(flag, "-a") == 0 || strcmp(flag, "-al") == 0 || strcmp(flag, "-la") == 0) {
                // Check for -a or -al combinations
                if (strcmp(flag, "-a") == 0) {
                    flags = 1; // -a flag
                    // Check if there's another flag
                    char *next_flag = strtok(NULL, " ");
                    if (next_flag) {
                        if (strcmp(next_flag, "-l") == 0) {
                            flags = 3; // -a -l = -al
                        } else {
                            valid_flags = 0;
                        }
                    }
                } else if (strcmp(flag, "-al") == 0 || strcmp(flag, "-la") == 0) {
                    flags = 3; // -al flag
                    // Should not have more flags
                    if (strtok(NULL, " ") != NULL) {
                        valid_flags = 0;
                    }
                }
            } else if (strcmp(flag, "-l") == 0) {
                // Check for -l or -l -a
                flags = 2; // -l flag
                char *next_flag = strtok(NULL, " ");
                if (next_flag) {
                    if (strcmp(next_flag, "-a") == 0) {
                        flags = 3; // -l -a = -al
                    } else {
                        valid_flags = 0;
                    }
                }
            } else {
                // Invalid flag
                valid_flags = 0;
            }
            
            if (!valid_flags) {
                printf("ERROR: Invalid VIEW command. Valid formats:\n");
                printf("  VIEW          - List files user has access to\n");
                printf("  VIEW -a       - List all files on system\n");
                printf("  VIEW -l       - List user files with details\n");
                printf("  VIEW -al      - List all files with details\n");
            } else {
                handle_view(flags);
            }
        } else if (strcmp(cmd, "READ") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) handle_read(filename);
        } else if (strcmp(cmd, "CREATE") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) handle_create(filename);
        } else if (strcmp(cmd, "WRITE") == 0) {
            char *filename = strtok(NULL, " ");
            char *sent_str = strtok(NULL, " ");
            if (filename && sent_str) {
                handle_write(filename, atoi(sent_str));
            }
        } else if (strcmp(cmd, "DELETE") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) handle_delete(filename);
        } else if (strcmp(cmd, "STREAM") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) handle_stream(filename);
        } else if (strcmp(cmd, "INFO") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) handle_info(filename);
        } else if (strcmp(cmd, "LIST") == 0) {
            handle_list_users();
        } else if (strcmp(cmd, "UNDO") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) handle_undo(filename);
        } else if (strcmp(cmd, "ADDACCESS") == 0) {
            char *flag = strtok(NULL, " ");
            char *filename = strtok(NULL, " ");
            char *target_user = strtok(NULL, " ");
            if (flag && filename && target_user) {
                handle_add_access(flag, filename, target_user);
            }
        } else if (strcmp(cmd, "REMACCESS") == 0) {
            char *filename = strtok(NULL, " ");
            char *target_user = strtok(NULL, " ");
            if (filename && target_user) {
                handle_rem_access(filename, target_user);
            }
        } else if (strcmp(cmd, "EXEC") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) handle_exec(filename);
        } else if (strcmp(cmd, "CREATEFOLDER") == 0) {
            char *foldername = strtok(NULL, " ");
            if (foldername) {
                handle_create_folder(foldername);
            } else {
                printf("ERROR: Usage: CREATEFOLDER <foldername>\n");
            }
        } else if (strcmp(cmd, "MOVE") == 0) {
            char *filename = strtok(NULL, " ");
            char *foldername = strtok(NULL, " ");
            if (filename && foldername) {
                handle_move_file(filename, foldername);
            } else {
                printf("ERROR: Usage: MOVE <filename> <foldername>\n");
            }
        } else if (strcmp(cmd, "VIEWFOLDER") == 0) {
            char *foldername = strtok(NULL, " ");
            if (foldername) {
                handle_view_folder(foldername);
            } else {
                printf("ERROR: Usage: VIEWFOLDER <foldername>\n");
            }
        } else if (strcmp(cmd, "CHECKPOINT") == 0) {
            char *filename = strtok(NULL, " ");
            char *tag = strtok(NULL, " ");
            if (filename && tag) {
                handle_checkpoint(filename, tag);
            } else {
                printf("ERROR: Usage: CHECKPOINT <filename> <tag>\n");
            }
        } else if (strcmp(cmd, "VIEWCHECKPOINT") == 0) {
            char *filename = strtok(NULL, " ");
            char *tag = strtok(NULL, " ");
            if (filename && tag) {
                handle_view_checkpoint(filename, tag);
            } else {
                printf("ERROR: Usage: VIEWCHECKPOINT <filename> <tag>\n");
            }
        } else if (strcmp(cmd, "REVERT") == 0) {
            char *filename = strtok(NULL, " ");
            char *tag = strtok(NULL, " ");
            if (filename && tag) {
                handle_revert_checkpoint(filename, tag);
            } else {
                printf("ERROR: Usage: REVERT <filename> <tag>\n");
            }
        } else if (strcmp(cmd, "LISTCHECKPOINTS") == 0) {
            char *filename = strtok(NULL, " ");
            if (filename) {
                handle_list_checkpoints(filename);
            } else {
                printf("ERROR: Usage: LISTCHECKPOINTS <filename>\n");
            }
        } else {
            printf("Unknown command. Type HELP for available commands.\n");
        }
    }
    
    printf("Goodbye!\n");
    return 0;
}