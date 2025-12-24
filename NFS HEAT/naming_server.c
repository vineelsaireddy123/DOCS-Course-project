#include "common.h"

#define ACCESS_CONTROL_FILE "access_control.dat"

// Global data structures
StorageServerInfo storage_servers[MAX_SS];
int num_ss = 0;
pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;

ClientInfo clients[MAX_CLIENTS];
int num_clients = 0;
pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;

AccessControl access_controls[MAX_FILES];
int num_access_controls = 0;
pthread_mutex_t access_lock = PTHREAD_MUTEX_INITIALIZER;

// Function to save access control to disk
void save_access_control() {
    pthread_mutex_lock(&access_lock);
    
    FILE *fp = fopen(ACCESS_CONTROL_FILE, "wb");
    if (fp) {
        fwrite(&num_access_controls, sizeof(int), 1, fp);
        fwrite(access_controls, sizeof(AccessControl), num_access_controls, fp);
        fclose(fp);
    }
    
    pthread_mutex_unlock(&access_lock);
}

// Function to load access control from disk
void load_access_control() {
    FILE *fp = fopen(ACCESS_CONTROL_FILE, "rb");
    if (fp) {
        pthread_mutex_lock(&access_lock);
        fread(&num_access_controls, sizeof(int), 1, fp);
        fread(access_controls, sizeof(AccessControl), num_access_controls, fp);
        pthread_mutex_unlock(&access_lock);
        fclose(fp);
        
        log_message("NM", "Access control data loaded from disk");
    }
}

// LRU Cache for file lookups
typedef struct CacheNode {
    char filename[MAX_FILENAME];
    int ss_index;
    time_t timestamp;
    struct CacheNode *next, *prev;
} CacheNode;

CacheNode *cache_head = NULL;
CacheNode *cache_tail = NULL;
int cache_size = 0;
#define MAX_CACHE_SIZE 100
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

// Trie for efficient file search
typedef struct TrieNode {
    struct TrieNode *children[256];
    int ss_index;
    int is_end;
} TrieNode;

TrieNode *trie_root = NULL;
pthread_mutex_t trie_lock = PTHREAD_MUTEX_INITIALIZER;

TrieNode* create_trie_node() {
    TrieNode *node = (TrieNode*)calloc(1, sizeof(TrieNode));
    node->ss_index = -1;
    return node;
}

void insert_trie(const char *filename, int ss_index) {
    pthread_mutex_lock(&trie_lock);
    if (!trie_root) trie_root = create_trie_node();
    
    TrieNode *curr = trie_root;
    for (int i = 0; filename[i]; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (!curr->children[c]) {
            curr->children[c] = create_trie_node();
        }
        curr = curr->children[c];
    }
    curr->is_end = 1;
    curr->ss_index = ss_index;
    pthread_mutex_unlock(&trie_lock);
}

int search_trie(const char *filename) {
    pthread_mutex_lock(&trie_lock);
    if (!trie_root) {
        pthread_mutex_unlock(&trie_lock);
        return -1;
    }
    
    TrieNode *curr = trie_root;
    for (int i = 0; filename[i]; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (!curr->children[c]) {
            pthread_mutex_unlock(&trie_lock);
            return -1;
        }
        curr = curr->children[c];
    }
    
    int result = (curr->is_end) ? curr->ss_index : -1;
    pthread_mutex_unlock(&trie_lock);
    return result;
}

void update_cache(const char *filename, int ss_index) {
    pthread_mutex_lock(&cache_lock);
    
    // Check if already in cache
    CacheNode *curr = cache_head;
    while (curr) {
        if (strcmp(curr->filename, filename) == 0) {
            // Move to front
            if (curr != cache_head) {
                if (curr->prev) curr->prev->next = curr->next;
                if (curr->next) curr->next->prev = curr->prev;
                if (curr == cache_tail) cache_tail = curr->prev;
                
                curr->next = cache_head;
                curr->prev = NULL;
                cache_head->prev = curr;
                cache_head = curr;
            }
            curr->timestamp = time(NULL);
            pthread_mutex_unlock(&cache_lock);
            return;
        }
        curr = curr->next;
    }
    
    // Add new node
    CacheNode *node = (CacheNode*)malloc(sizeof(CacheNode));
    strcpy(node->filename, filename);
    node->ss_index = ss_index;
    node->timestamp = time(NULL);
    node->prev = NULL;
    node->next = cache_head;
    
    if (cache_head) cache_head->prev = node;
    cache_head = node;
    if (!cache_tail) cache_tail = node;
    cache_size++;
    
    // Evict if necessary
    if (cache_size > MAX_CACHE_SIZE) {
        CacheNode *old = cache_tail;
        cache_tail = old->prev;
        if (cache_tail) cache_tail->next = NULL;
        free(old);
        cache_size--;
    }
    
    pthread_mutex_unlock(&cache_lock);
}

int find_file_ss(const char *filename) {
    // Check cache first
    pthread_mutex_lock(&cache_lock);
    CacheNode *curr = cache_head;
    while (curr) {
        if (strcmp(curr->filename, filename) == 0) {
            int ss_idx = curr->ss_index;
            pthread_mutex_unlock(&cache_lock);
            return ss_idx;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&cache_lock);
    
    // Search trie
    int ss_idx = search_trie(filename);
    if (ss_idx >= 0) {
        update_cache(filename, ss_idx);
        return ss_idx;
    }
    
    // Linear search as fallback
    pthread_mutex_lock(&ss_lock);
    for (int i = 0; i < num_ss; i++) {
        if (!storage_servers[i].active) continue;
        for (int j = 0; j < storage_servers[i].num_files; j++) {
            if (strcmp(storage_servers[i].files[j], filename) == 0) {
                pthread_mutex_unlock(&ss_lock);
                update_cache(filename, i);
                insert_trie(filename, i);
                return i;
            }
        }
    }
    pthread_mutex_unlock(&ss_lock);
    return -1;
}

int check_access(const char *filename, const char *username, int required_level) {
    pthread_mutex_lock(&access_lock);
    
    for (int i = 0; i < num_access_controls; i++) {
        if (strcmp(access_controls[i].filename, filename) == 0) {
            for (int j = 0; j < access_controls[i].num_entries; j++) {
                if (strcmp(access_controls[i].entries[j].username, username) == 0) {
                    int has_access = access_controls[i].entries[j].access_level >= required_level;
                    pthread_mutex_unlock(&access_lock);
                    return has_access;
                }
            }
            pthread_mutex_unlock(&access_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&access_lock);
    return 0;
}

void *handle_ss_registration(void *arg) {
    int sockfd = *(int*)arg;
    free(arg);
    
    Message msg;
    if (receive_message(sockfd, &msg) < 0) {
        close(sockfd);
        return NULL;
    }
    
    pthread_mutex_lock(&ss_lock);
    if (num_ss >= MAX_SS) {
        pthread_mutex_unlock(&ss_lock);
        msg.type = MSG_ERROR;
        msg.error_code = ERR_SS_UNAVAILABLE;
        send_message(sockfd, &msg);
        close(sockfd);
        return NULL;
    }
    
    int ss_idx = num_ss++;
    strcpy(storage_servers[ss_idx].ip, msg.ss_ip);
    storage_servers[ss_idx].nm_port = msg.ss_port;
    storage_servers[ss_idx].client_port = msg.flags; // Using flags field
    storage_servers[ss_idx].active = 1;
    pthread_mutex_init(&storage_servers[ss_idx].lock, NULL);
    
    // Parse file list from data field
    char *token = strtok(msg.data, "\n");
    storage_servers[ss_idx].num_files = 0;
    while (token && storage_servers[ss_idx].num_files < MAX_FILES) {
        strcpy(storage_servers[ss_idx].files[storage_servers[ss_idx].num_files++], token);
        insert_trie(token, ss_idx);
        token = strtok(NULL, "\n");
    }
    
    pthread_mutex_unlock(&ss_lock);
    
    log_message("NM", "Storage Server registered successfully");
    
    msg.type = MSG_ACK;
    send_message(sockfd, &msg);
    
    close(sockfd);
    return NULL;
}

void *handle_client(void *arg) {
    int sockfd = *(int*)arg;
    free(arg);
    
    Message msg;
    while (1) {
        if (receive_message(sockfd, &msg) < 0) {
            break;
        }
        
        log_request("NM", msg.ss_ip, msg.ss_port, "Client request");
        
        Message response;
        init_message(&response);
        
        switch (msg.type) {
            case MSG_REGISTER_CLIENT: {
                // Register client
                pthread_mutex_lock(&client_lock);
                if (num_clients < MAX_CLIENTS) {
                    strcpy(clients[num_clients].username, msg.username);
                    clients[num_clients].active = 1;
                    num_clients++;
                }
                pthread_mutex_unlock(&client_lock);
                
                response.type = MSG_ACK;
                send_message(sockfd, &response);
                log_message("NM", "Client registered");
                break;
            }
            
            case MSG_LIST_FILES: {
                pthread_mutex_lock(&ss_lock);
                response.type = MSG_RESPONSE;
                response.data[0] = '\0';
                
                // Use a temporary array to track unique files
                char unique_files[MAX_FILES][MAX_FILENAME];
                int num_unique = 0;
                
                for (int i = 0; i < num_ss; i++) {
                    if (!storage_servers[i].active) continue;
                    for (int j = 0; j < storage_servers[i].num_files; j++) {
                        // Check if user has access or if -a flag is set
                        if (msg.flags == 1 || check_access(storage_servers[i].files[j], 
                                                           msg.username, ACCESS_READ)) {
                            // Check if file is already in unique list
                            int is_duplicate = 0;
                            for (int k = 0; k < num_unique; k++) {
                                if (strcmp(unique_files[k], storage_servers[i].files[j]) == 0) {
                                    is_duplicate = 1;
                                    break;
                                }
                            }
                            
                            if (!is_duplicate) {
                                strcpy(unique_files[num_unique++], storage_servers[i].files[j]);
                                strcat(response.data, storage_servers[i].files[j]);
                                strcat(response.data, "\n");
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&ss_lock);
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_READ_FILE:
            case MSG_WRITE_FILE:
            case MSG_STREAM_FILE: {
                int ss_idx = find_file_ss(msg.filename);
                if (ss_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_FILE_NOT_FOUND;
                } else if (!check_access(msg.filename, msg.username, 
                          (msg.type == MSG_WRITE_FILE) ? ACCESS_WRITE : ACCESS_READ)) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_UNAUTHORIZED;
                } else {
                    response.type = MSG_RESPONSE;
                    strcpy(response.ss_ip, storage_servers[ss_idx].ip);
                    response.ss_port = storage_servers[ss_idx].client_port;
                }
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_CREATE_FILE: {
                // Forward to first available SS
                int ss_idx = -1;
                pthread_mutex_lock(&ss_lock);
                for (int i = 0; i < num_ss; i++) {
                    if (storage_servers[i].active) {
                        ss_idx = i;
                        break;
                    }
                }
                pthread_mutex_unlock(&ss_lock);
                
                if (ss_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SS_UNAVAILABLE;
                    send_message(sockfd, &response);
                } else {
                    // Connect to SS and forward request
                    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in ss_addr;
                    ss_addr.sin_family = AF_INET;
                    ss_addr.sin_port = htons(storage_servers[ss_idx].nm_port);
                    inet_pton(AF_INET, storage_servers[ss_idx].ip, &ss_addr.sin_addr);
                    
                    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                        send_message(ss_sock, &msg);
                        receive_message(ss_sock, &response);
                        
                        if (response.type == MSG_ACK) {
                            pthread_mutex_lock(&ss_lock);
                            strcpy(storage_servers[ss_idx].files[storage_servers[ss_idx].num_files++], 
                                   msg.filename);
                            pthread_mutex_unlock(&ss_lock);
                            insert_trie(msg.filename, ss_idx);
                            
                            // Add owner to access control
                            pthread_mutex_lock(&access_lock);
                            strcpy(access_controls[num_access_controls].filename, msg.filename);
                            strcpy(access_controls[num_access_controls].entries[0].username, msg.username);
                            access_controls[num_access_controls].entries[0].access_level = ACCESS_WRITE;
                            access_controls[num_access_controls].num_entries = 1;
                            num_access_controls++;
                            pthread_mutex_unlock(&access_lock);
                            save_access_control();
                        }
                        close(ss_sock);
                    } else {
                        response.type = MSG_ERROR;
                        response.error_code = ERR_SS_UNAVAILABLE;
                    }
                    send_message(sockfd, &response);
                }
                break;
            }
            
            case MSG_DELETE_FILE: {
                // Check if user is owner
                int is_owner = 0;
                pthread_mutex_lock(&access_lock);
                for (int i = 0; i < num_access_controls; i++) {
                    if (strcmp(access_controls[i].filename, msg.filename) == 0) {
                        if (strcmp(access_controls[i].entries[0].username, msg.username) == 0) {
                            is_owner = 1;
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&access_lock);
                
                if (!is_owner) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_PERMISSION_DENIED;
                    send_message(sockfd, &response);
                    break;
                }
                
                // Find SS with file and forward delete
                int ss_idx = find_file_ss(msg.filename);
                if (ss_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_FILE_NOT_FOUND;
                    send_message(sockfd, &response);
                    break;
                }
                
                // Connect to SS and forward request
                int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in ss_addr;
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(storage_servers[ss_idx].nm_port);
                inet_pton(AF_INET, storage_servers[ss_idx].ip, &ss_addr.sin_addr);
                
                if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                    send_message(ss_sock, &msg);
                    receive_message(ss_sock, &response);
                    
                    if (response.type == MSG_ACK) {
                        // Remove from SS file list
                        pthread_mutex_lock(&ss_lock);
                        for (int i = 0; i < storage_servers[ss_idx].num_files; i++) {
                            if (strcmp(storage_servers[ss_idx].files[i], msg.filename) == 0) {
                                for (int j = i; j < storage_servers[ss_idx].num_files - 1; j++) {
                                    strcpy(storage_servers[ss_idx].files[j], 
                                           storage_servers[ss_idx].files[j + 1]);
                                }
                                storage_servers[ss_idx].num_files--;
                                break;
                            }
                        }
                        pthread_mutex_unlock(&ss_lock);
                        
                        // Remove from access control
                        pthread_mutex_lock(&access_lock);
                        for (int i = 0; i < num_access_controls; i++) {
                            if (strcmp(access_controls[i].filename, msg.filename) == 0) {
                                for (int j = i; j < num_access_controls - 1; j++) {
                                    access_controls[j] = access_controls[j + 1];
                                }
                                num_access_controls--;
                                break;
                            }
                        }
                        pthread_mutex_unlock(&access_lock);
                        save_access_control();
                    }
                    close(ss_sock);
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SS_UNAVAILABLE;
                }
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_LIST_USERS: {
                response.type = MSG_RESPONSE;
                response.data[0] = '\0';
                
                // Use array to track unique usernames
                char unique_users[MAX_CLIENTS][MAX_USERNAME];
                int num_unique = 0;
                
                // Add from client list
                pthread_mutex_lock(&client_lock);
                for (int i = 0; i < num_clients; i++) {
                    int is_duplicate = 0;
                    for (int j = 0; j < num_unique; j++) {
                        if (strcmp(unique_users[j], clients[i].username) == 0) {
                            is_duplicate = 1;
                            break;
                        }
                    }
                    if (!is_duplicate && num_unique < MAX_CLIENTS) {
                        strcpy(unique_users[num_unique++], clients[i].username);
                    }
                }
                pthread_mutex_unlock(&client_lock);
                
                // Add from access control
                pthread_mutex_lock(&access_lock);
                for (int i = 0; i < num_access_controls; i++) {
                    for (int j = 0; j < access_controls[i].num_entries; j++) {
                        int is_duplicate = 0;
                        for (int k = 0; k < num_unique; k++) {
                            if (strcmp(unique_users[k], access_controls[i].entries[j].username) == 0) {
                                is_duplicate = 1;
                                break;
                            }
                        }
                        if (!is_duplicate && num_unique < MAX_CLIENTS) {
                            strcpy(unique_users[num_unique++], access_controls[i].entries[j].username);
                        }
                    }
                }
                pthread_mutex_unlock(&access_lock);
                
                // Build response
                for (int i = 0; i < num_unique; i++) {
                    strcat(response.data, unique_users[i]);
                    strcat(response.data, "\n");
                }
                
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_ADD_ACCESS: {
                // Check if requester is owner
                int is_owner = 0;
                pthread_mutex_lock(&access_lock);
                for (int i = 0; i < num_access_controls; i++) {
                    if (strcmp(access_controls[i].filename, msg.filename) == 0) {
                        if (strcmp(access_controls[i].entries[0].username, msg.username) == 0) {
                            is_owner = 1;
                        }
                        
                        if (is_owner) {
                            // Check if user already has access
                            int found = -1;
                            for (int j = 0; j < access_controls[i].num_entries; j++) {
                                if (strcmp(access_controls[i].entries[j].username, msg.data) == 0) {
                                    found = j;
                                    break;
                                }
                            }
                            
                            if (found >= 0) {
                                // Update existing access
                                access_controls[i].entries[found].access_level = 
                                    (msg.flags == 1) ? ACCESS_READ : ACCESS_WRITE;
                            } else {
                                // Add new access
                                int idx = access_controls[i].num_entries++;
                                strcpy(access_controls[i].entries[idx].username, msg.data);
                                access_controls[i].entries[idx].access_level = 
                                    (msg.flags == 1) ? ACCESS_READ : ACCESS_WRITE;
                            }
                            response.type = MSG_ACK;
                        } else {
                            response.type = MSG_ERROR;
                            response.error_code = ERR_PERMISSION_DENIED;
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&access_lock);
                save_access_control();
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_REM_ACCESS: {
                // Check if requester is owner
                int is_owner = 0;
                pthread_mutex_lock(&access_lock);
                for (int i = 0; i < num_access_controls; i++) {
                    if (strcmp(access_controls[i].filename, msg.filename) == 0) {
                        if (strcmp(access_controls[i].entries[0].username, msg.username) == 0) {
                            is_owner = 1;
                        }
                        
                        if (is_owner) {
                            // Remove access
                            for (int j = 1; j < access_controls[i].num_entries; j++) {
                                if (strcmp(access_controls[i].entries[j].username, msg.data) == 0) {
                                    // Shift entries
                                    for (int k = j; k < access_controls[i].num_entries - 1; k++) {
                                        access_controls[i].entries[k] = access_controls[i].entries[k + 1];
                                    }
                                    access_controls[i].num_entries--;
                                    break;
                                }
                            }
                            response.type = MSG_ACK;
                        } else {
                            response.type = MSG_ERROR;
                            response.error_code = ERR_PERMISSION_DENIED;
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&access_lock);
                save_access_control();
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_EXEC_FILE: {
                int ss_idx = find_file_ss(msg.filename);
                if (ss_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_FILE_NOT_FOUND;
                    send_message(sockfd, &response);
                    break;
                }
                
                if (!check_access(msg.filename, msg.username, ACCESS_READ)) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_UNAUTHORIZED;
                    send_message(sockfd, &response);
                    break;
                }
                
                // Get file content from SS
                int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in ss_addr;
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(storage_servers[ss_idx].nm_port);
                inet_pton(AF_INET, storage_servers[ss_idx].ip, &ss_addr.sin_addr);
                
                if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                    Message ss_msg;
                    init_message(&ss_msg);
                    ss_msg.type = MSG_READ_FILE;
                    strcpy(ss_msg.filename, msg.filename);
                    send_message(ss_sock, &ss_msg);
                    
                    Message ss_resp;
                    receive_message(ss_sock, &ss_resp);
                    close(ss_sock);
                    
                    if (ss_resp.type == MSG_RESPONSE) {
                        // Execute commands
                        FILE *fp = popen(ss_resp.data, "r");
                        if (fp) {
                            response.data[0] = '\0';
                            char line[256];
                            while (fgets(line, sizeof(line), fp)) {
                                if (strlen(response.data) + strlen(line) < sizeof(response.data)) {
                                    strcat(response.data, line);
                                }
                            }
                            pclose(fp);
                            response.type = MSG_RESPONSE;
                        } else {
                            response.type = MSG_ERROR;
                            response.error_code = ERR_INVALID_COMMAND;
                        }
                    } else {
                        response.type = MSG_ERROR;
                        response.error_code = ERR_FILE_NOT_FOUND;
                    }
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SS_UNAVAILABLE;
                }
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_GET_OWNER: {
                response.type = MSG_RESPONSE;
                response.data[0] = '\0';
                
                // Find owner from access control
                pthread_mutex_lock(&access_lock);
                for (int i = 0; i < num_access_controls; i++) {
                    if (strcmp(access_controls[i].filename, msg.filename) == 0) {
                        // Owner is always the first entry
                        strcpy(response.data, access_controls[i].entries[0].username);
                        break;
                    }
                }
                pthread_mutex_unlock(&access_lock);
                
                send_message(sockfd, &response);
                break;
            }
            
            // ===== FOLDER OPERATIONS =====
            case MSG_CREATE_FOLDER: {
                // Forward to a storage server
                int ss_idx = 0; // Use first active storage server
                pthread_mutex_lock(&ss_lock);
                for (int i = 0; i < num_ss; i++) {
                    if (storage_servers[i].active) {
                        ss_idx = i;
                        break;
                    }
                }
                pthread_mutex_unlock(&ss_lock);
                
                if (ss_idx >= 0) {
                    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in ss_addr;
                    ss_addr.sin_family = AF_INET;
                    ss_addr.sin_port = htons(storage_servers[ss_idx].client_port);
                    inet_pton(AF_INET, storage_servers[ss_idx].ip, &ss_addr.sin_addr);
                    
                    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                        send_message(ss_sock, &msg);
                        receive_message(ss_sock, &response);
                        close(ss_sock);
                    } else {
                        response.type = MSG_ERROR;
                        response.error_code = ERR_SS_UNAVAILABLE;
                    }
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SS_UNAVAILABLE;
                }
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_MOVE_FILE: {
                // Find SS containing the file
                int ss_idx = find_file_ss(msg.filename);
                if (ss_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_FILE_NOT_FOUND;
                    send_message(sockfd, &response);
                    break;
                }
                
                // Check access
                if (!check_access(msg.filename, msg.username, ACCESS_WRITE)) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_UNAUTHORIZED;
                    send_message(sockfd, &response);
                    break;
                }
                
                // Forward to the storage server
                int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in ss_addr;
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(storage_servers[ss_idx].client_port);
                inet_pton(AF_INET, storage_servers[ss_idx].ip, &ss_addr.sin_addr);
                
                if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                    send_message(ss_sock, &msg);
                    receive_message(ss_sock, &response);
                    close(ss_sock);
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SS_UNAVAILABLE;
                }
                send_message(sockfd, &response);
                break;
            }
            
            case MSG_VIEW_FOLDER: {
                // Use first active storage server to view folder
                int ss_idx = 0;
                pthread_mutex_lock(&ss_lock);
                for (int i = 0; i < num_ss; i++) {
                    if (storage_servers[i].active) {
                        ss_idx = i;
                        break;
                    }
                }
                pthread_mutex_unlock(&ss_lock);
                
                if (ss_idx >= 0) {
                    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in ss_addr;
                    ss_addr.sin_family = AF_INET;
                    ss_addr.sin_port = htons(storage_servers[ss_idx].client_port);
                    inet_pton(AF_INET, storage_servers[ss_idx].ip, &ss_addr.sin_addr);
                    
                    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                        send_message(ss_sock, &msg);
                        receive_message(ss_sock, &response);
                        close(ss_sock);
                    } else {
                        response.type = MSG_ERROR;
                        response.error_code = ERR_SS_UNAVAILABLE;
                    }
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SS_UNAVAILABLE;
                }
                send_message(sockfd, &response);
                break;
            }
            // ===== END FOLDER OPERATIONS =====
            
            // ===== CHECKPOINT OPERATIONS =====
            case MSG_CHECKPOINT:
            case MSG_VIEWCHECKPOINT:
            case MSG_REVERT:
            case MSG_LISTCHECKPOINTS: {
                // Find SS containing the file
                int ss_idx = find_file_ss(msg.filename);
                if (ss_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_FILE_NOT_FOUND;
                    send_message(sockfd, &response);
                    break;
                }
                
                // Check access based on operation type
                int required_level = (msg.type == MSG_CHECKPOINT) ? ACCESS_WRITE : ACCESS_READ;
                if (!check_access(msg.filename, msg.username, required_level)) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_UNAUTHORIZED;
                    send_message(sockfd, &response);
                    break;
                }
                
                // Forward to the storage server
                int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in ss_addr;
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(storage_servers[ss_idx].client_port);
                inet_pton(AF_INET, storage_servers[ss_idx].ip, &ss_addr.sin_addr);
                
                if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                    send_message(ss_sock, &msg);
                    receive_message(ss_sock, &response);
                    close(ss_sock);
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SS_UNAVAILABLE;
                }
                send_message(sockfd, &response);
                break;
            }
            // ===== END CHECKPOINT OPERATIONS =====
        }
    }
    
    close(sockfd);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        return 1;
    }
    
    if (listen(server_fd, 50) < 0) {
        perror("Listen failed");
        return 1;
    }
    
    log_message("NM", "Naming Server started");
    printf("Naming Server listening on port %d\n", port);
    
    load_access_control();
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &len);
        
        if (*client_sock < 0) {
            free(client_sock);
            continue;
        }
        
        // Peek at message type to determine handler
        Message peek_msg;
        if (recv(*client_sock, &peek_msg, sizeof(Message), MSG_PEEK) > 0) {
            pthread_t tid;
            if (peek_msg.type == MSG_REGISTER_SS) {
                pthread_create(&tid, NULL, handle_ss_registration, client_sock);
            } else {
                pthread_create(&tid, NULL, handle_client, client_sock);
            }
            pthread_detach(tid);
        } else {
            close(*client_sock);
            free(client_sock);
        }
    }
    
    return 0;
}