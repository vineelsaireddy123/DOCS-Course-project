#include "common.h"

char storage_dir[MAX_PATH];
pthread_mutex_t file_locks[MAX_FILES];
int sentence_locks[MAX_FILES][1000]; // sentence_locks[file_idx][sentence_num] = client_id
pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;

// File-level write locks - when set, no one can read or write
typedef struct {
    char filename[MAX_FILENAME];
    int locked;
    pthread_mutex_t mutex;
} FileLock;

FileLock file_write_locks[MAX_FILES];
int file_lock_count = 0;
pthread_mutex_t file_lock_list_mutex = PTHREAD_MUTEX_INITIALIZER;

int nm_port_listen; // Port for NM commands
int client_port_listen; // Port for client operations

typedef struct {
    char filename[MAX_FILENAME];
    char content[MAX_BUFFER * 4];
    time_t timestamp;
} UndoEntry;

UndoEntry undo_history[MAX_FILES];
int undo_count = 0;
pthread_mutex_t undo_lock = PTHREAD_MUTEX_INITIALIZER;

void init_storage() {
    mkdir(storage_dir, 0755);
    for (int i = 0; i < MAX_FILES; i++) {
        pthread_mutex_init(&file_locks[i], NULL);
        pthread_mutex_init(&file_write_locks[i].mutex, NULL);
        file_write_locks[i].locked = 0;
        file_write_locks[i].filename[0] = '\0';
        for (int j = 0; j < 1000; j++) {
            sentence_locks[i][j] = -1;
        }
    }
}

int lock_file_for_write(const char *filename) {
    pthread_mutex_lock(&file_lock_list_mutex);
    
    // Find existing lock or create new one
    int idx = -1;
    for (int i = 0; i < file_lock_count; i++) {
        if (strcmp(file_write_locks[i].filename, filename) == 0) {
            idx = i;
            break;
        }
    }
    
    if (idx == -1 && file_lock_count < MAX_FILES) {
        idx = file_lock_count++;
        strcpy(file_write_locks[idx].filename, filename);
        file_write_locks[idx].locked = 0;
    }
    
    if (idx == -1) {
        pthread_mutex_unlock(&file_lock_list_mutex);
        return -1;
    }
    
    // Now check and acquire the lock atomically
    pthread_mutex_lock(&file_write_locks[idx].mutex);
    pthread_mutex_unlock(&file_lock_list_mutex);
    
    if (file_write_locks[idx].locked) {
        pthread_mutex_unlock(&file_write_locks[idx].mutex);
        return -1; // Already locked
    }
    
    file_write_locks[idx].locked = 1;
    pthread_mutex_unlock(&file_write_locks[idx].mutex);
    
    return idx;
}

void unlock_file_for_write(int lock_idx) {
    if (lock_idx >= 0 && lock_idx < file_lock_count) {
        pthread_mutex_lock(&file_write_locks[lock_idx].mutex);
        file_write_locks[lock_idx].locked = 0;
        pthread_mutex_unlock(&file_write_locks[lock_idx].mutex);
    }
}

int is_file_locked_for_write(const char *filename) {
    pthread_mutex_lock(&file_lock_list_mutex);
    
    for (int i = 0; i < file_lock_count; i++) {
        if (strcmp(file_write_locks[i].filename, filename) == 0) {
            pthread_mutex_unlock(&file_lock_list_mutex);
            
            pthread_mutex_lock(&file_write_locks[i].mutex);
            int locked = file_write_locks[i].locked;
            pthread_mutex_unlock(&file_write_locks[i].mutex);
            
            return locked;
        }
    }
    
    pthread_mutex_unlock(&file_lock_list_mutex);
    return 0;
}

int get_file_index(const char *filename) {
    // Simple hash for demonstration
    unsigned long hash = 5381;
    int c;
    const char *str = filename;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % MAX_FILES;
}

void save_undo(const char *filename, const char *content) {
    pthread_mutex_lock(&undo_lock);
    int idx = undo_count % MAX_FILES;
    strcpy(undo_history[idx].filename, filename);
    strncpy(undo_history[idx].content, content, sizeof(undo_history[idx].content) - 1);
    undo_history[idx].timestamp = time(NULL);
    undo_count++;
    pthread_mutex_unlock(&undo_lock);
}

// ===== FOLDER MANAGEMENT FUNCTIONS =====

int create_folder(const char *folder_path) {
    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", storage_dir, folder_path);
    
    // Create directory recursively
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", full_path);
    
    for (char *p = path + strlen(storage_dir) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
    
    if (mkdir(full_path, 0755) == 0 || errno == EEXIST) {
        return 0; // Success
    }
    return -1; // Failed
}

int folder_exists(const char *folder_path) {
    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", storage_dir, folder_path);
    
    struct stat st;
    return (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

int move_file_to_folder(const char *filename, const char *folder_path) {
    char old_path[MAX_PATH];
    char new_path[MAX_PATH];
    
    snprintf(old_path, sizeof(old_path), "%s/%s", storage_dir, filename);
    snprintf(new_path, sizeof(new_path), "%s/%s/%s", storage_dir, folder_path, filename);
    
    // Check if folder exists
    if (!folder_exists(folder_path)) {
        return -1; // Folder doesn't exist
    }
    
    // Check if file exists
    struct stat st;
    if (stat(old_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return -1; // File doesn't exist or is a directory
    }
    
    // Move file
    if (rename(old_path, new_path) == 0) {
        return 0; // Success
    }
    return -1; // Failed
}

int list_folder_contents(const char *folder_path, char *buffer, size_t buf_size) {
    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", storage_dir, folder_path);
    
    DIR *dir = opendir(full_path);
    if (!dir) {
        return -1; // Folder doesn't exist
    }
    
    buffer[0] = '\0';
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char entry_path[MAX_PATH];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, entry->d_name);
        
        struct stat st;
        if (stat(entry_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                strncat(buffer, "[FOLDER] ", buf_size - strlen(buffer) - 1);
            } else {
                strncat(buffer, "[FILE] ", buf_size - strlen(buffer) - 1);
            }
            strncat(buffer, entry->d_name, buf_size - strlen(buffer) - 1);
            strncat(buffer, "\n", buf_size - strlen(buffer) - 1);
        }
    }
    
    closedir(dir);
    return 0; // Success
}

// ===== END FOLDER MANAGEMENT FUNCTIONS =====

// ===== CHECKPOINT MANAGEMENT FUNCTIONS =====

typedef struct {
    char filename[MAX_FILENAME];
    Checkpoint checkpoints[MAX_CHECKPOINTS];
    int num_checkpoints;
    pthread_mutex_t lock;
} CheckpointStorage;

#define MAX_CHECKPOINT_FILES 100
CheckpointStorage checkpoint_storage[MAX_CHECKPOINT_FILES];
int checkpoint_storage_count = 0;
pthread_mutex_t checkpoint_storage_lock = PTHREAD_MUTEX_INITIALIZER;

void init_checkpoints() {
    pthread_mutex_lock(&checkpoint_storage_lock);
    checkpoint_storage_count = 0;
    for (int i = 0; i < MAX_CHECKPOINT_FILES; i++) {
        pthread_mutex_init(&checkpoint_storage[i].lock, NULL);
        checkpoint_storage[i].filename[0] = '\0';
        checkpoint_storage[i].num_checkpoints = 0;
    }
    pthread_mutex_unlock(&checkpoint_storage_lock);
}

int get_checkpoint_storage_index(const char *filename) {
    pthread_mutex_lock(&checkpoint_storage_lock);
    
    // Find existing entry
    for (int i = 0; i < checkpoint_storage_count; i++) {
        if (strcmp(checkpoint_storage[i].filename, filename) == 0) {
            pthread_mutex_unlock(&checkpoint_storage_lock);
            return i;
        }
    }
    
    // Create new entry
    if (checkpoint_storage_count < MAX_CHECKPOINT_FILES) {
        strcpy(checkpoint_storage[checkpoint_storage_count].filename, filename);
        checkpoint_storage[checkpoint_storage_count].num_checkpoints = 0;
        int idx = checkpoint_storage_count++;
        pthread_mutex_unlock(&checkpoint_storage_lock);
        return idx;
    }
    
    pthread_mutex_unlock(&checkpoint_storage_lock);
    return -1;
}

int create_checkpoint(const char *filename, const char *tag, const char *content, const char *username) {
    int idx = get_checkpoint_storage_index(filename);
    if (idx < 0) return -1;
    
    pthread_mutex_lock(&checkpoint_storage[idx].lock);
    
    // Check if tag already exists
    for (int i = 0; i < checkpoint_storage[idx].num_checkpoints; i++) {
        if (strcmp(checkpoint_storage[idx].checkpoints[i].tag, tag) == 0) {
            pthread_mutex_unlock(&checkpoint_storage[idx].lock);
            return -1; // Tag already exists
        }
    }
    
    // Check if we have space
    if (checkpoint_storage[idx].num_checkpoints >= MAX_CHECKPOINTS) {
        pthread_mutex_unlock(&checkpoint_storage[idx].lock);
        return -1; // No space for more checkpoints
    }
    
    // Create new checkpoint
    Checkpoint *cp = &checkpoint_storage[idx].checkpoints[checkpoint_storage[idx].num_checkpoints];
    strcpy(cp->tag, tag);
    strncpy(cp->content, content, sizeof(cp->content) - 1);
    cp->content[sizeof(cp->content) - 1] = '\0';
    cp->timestamp = time(NULL);
    strcpy(cp->username, username);
    
    checkpoint_storage[idx].num_checkpoints++;
    
    pthread_mutex_unlock(&checkpoint_storage[idx].lock);
    return 0;
}

int view_checkpoint(const char *filename, const char *tag, char *buffer, size_t buf_size) {
    int idx = get_checkpoint_storage_index(filename);
    if (idx < 0) return -1;
    
    pthread_mutex_lock(&checkpoint_storage[idx].lock);
    
    for (int i = 0; i < checkpoint_storage[idx].num_checkpoints; i++) {
        if (strcmp(checkpoint_storage[idx].checkpoints[i].tag, tag) == 0) {
            strncpy(buffer, checkpoint_storage[idx].checkpoints[i].content, buf_size - 1);
            buffer[buf_size - 1] = '\0';
            pthread_mutex_unlock(&checkpoint_storage[idx].lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&checkpoint_storage[idx].lock);
    return -1; // Checkpoint not found
}

int revert_checkpoint(const char *filename, const char *tag) {
    int idx = get_checkpoint_storage_index(filename);
    if (idx < 0) return -1;
    
    pthread_mutex_lock(&checkpoint_storage[idx].lock);
    
    // Find checkpoint
    for (int i = 0; i < checkpoint_storage[idx].num_checkpoints; i++) {
        if (strcmp(checkpoint_storage[idx].checkpoints[i].tag, tag) == 0) {
            // Revert file content
            char filepath[MAX_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, filename);
            
            FILE *fp = fopen(filepath, "w");
            if (!fp) {
                pthread_mutex_unlock(&checkpoint_storage[idx].lock);
                return -1;
            }
            
            fprintf(fp, "%s", checkpoint_storage[idx].checkpoints[i].content);
            fclose(fp);
            
            pthread_mutex_unlock(&checkpoint_storage[idx].lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&checkpoint_storage[idx].lock);
    return -1; // Checkpoint not found
}

int list_checkpoints(const char *filename, char *buffer, size_t buf_size) {
    int idx = get_checkpoint_storage_index(filename);
    if (idx < 0) return -1;
    
    pthread_mutex_lock(&checkpoint_storage[idx].lock);
    
    buffer[0] = '\0';
    for (int i = 0; i < checkpoint_storage[idx].num_checkpoints; i++) {
        Checkpoint *cp = &checkpoint_storage[idx].checkpoints[i];
        char timestamp_str[64];
        strftime(timestamp_str, sizeof(timestamp_str), "%Y-%m-%d %H:%M:%S", localtime(&cp->timestamp));
        
        char line[512];
        snprintf(line, sizeof(line), "[%d] Tag: %s | By: %s | Time: %s\n", 
                 i + 1, cp->tag, cp->username, timestamp_str);
        strncat(buffer, line, buf_size - strlen(buffer) - 1);
    }
    
    pthread_mutex_unlock(&checkpoint_storage[idx].lock);
    return 0;
}

// ===== END CHECKPOINT MANAGEMENT FUNCTIONS =====

int read_file_content(const char *filename, char *buffer, size_t buf_size) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, filename);
    
    FILE *fp = fopen(filepath, "r");
    if (!fp) return -1;
    
    size_t bytes_read = fread(buffer, 1, buf_size - 1, fp);
    buffer[bytes_read] = '\0';
    fclose(fp);
    return 0;
}

int write_file_content(const char *filename, const char *content) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, filename);
    
    FILE *fp = fopen(filepath, "w");
    if (!fp) return -1;
    
    fprintf(fp, "%s", content);
    fclose(fp);
    return 0;
}

void split_into_sentences(const char *content, char sentences[][MAX_SENTENCE_LEN], int *num_sentences) {
    *num_sentences = 0;
    int sent_idx = 0;
    int char_idx = 0;
    
    for (int i = 0; content[i]; i++) {
        sentences[sent_idx][char_idx++] = content[i];
        
        if (content[i] == '.' || content[i] == '!' || content[i] == '?') {
            sentences[sent_idx][char_idx] = '\0';
            sent_idx++;
            char_idx = 0;
            
            // Skip whitespace
            while (content[i + 1] == ' ') i++;
        }
    }
    
    if (char_idx > 0) {
        sentences[sent_idx][char_idx] = '\0';
        sent_idx++;
    }
    
    *num_sentences = sent_idx;
}

void split_into_words(const char *sentence, char words[][MAX_FILENAME], int *num_words) {
    *num_words = 0;
    int word_idx = 0;
    int char_idx = 0;
    
    for (int i = 0; sentence[i]; i++) {
        if (sentence[i] == ' ') {
            if (char_idx > 0) {
                words[word_idx][char_idx] = '\0';
                word_idx++;
                char_idx = 0;
            }
        } else {
            words[word_idx][char_idx++] = sentence[i];
        }
    }
    
    if (char_idx > 0) {
        words[word_idx][char_idx] = '\0';
        word_idx++;
    }
    
    *num_words = word_idx;
}

void reconstruct_sentence(char words[][MAX_FILENAME], int num_words, char *sentence) {
    sentence[0] = '\0';
    for (int i = 0; i < num_words; i++) {
        if (i > 0) strcat(sentence, " ");
        strcat(sentence, words[i]);
    }
}

void *handle_nm_request(void *arg) {
    int sockfd = *(int*)arg;
    free(arg);
    
    Message msg;
    if (receive_message(sockfd, &msg) < 0) {
        close(sockfd);
        return NULL;
    }
    
    Message response;
    init_message(&response);
    
    switch (msg.type) {
        case MSG_CREATE_FILE: {
            char filepath[MAX_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, msg.filename);
            
            // Check if file already exists
            if (access(filepath, F_OK) == 0) {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_EXISTS;
                strcpy(response.data, "File already exists");
                log_message("SS", "File creation failed - file already exists");
            } else {
                FILE *fp = fopen(filepath, "w");
                if (fp) {
                    fclose(fp);
                    response.type = MSG_ACK;
                    log_message("SS", "File created successfully");
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_FILE_EXISTS;
                }
            }
            break;
        }
        
        case MSG_DELETE_FILE: {
            char filepath[MAX_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, msg.filename);
            
            if (remove(filepath) == 0) {
                response.type = MSG_ACK;
                log_message("SS", "File deleted successfully");
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
            }
            break;
        }
        
        case MSG_READ_FILE: {
            if (read_file_content(msg.filename, response.data, sizeof(response.data)) == 0) {
                response.type = MSG_RESPONSE;
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
            }
            break;
        }
    }
    
    send_message(sockfd, &response);
    close(sockfd);
    return NULL;
}

void *handle_client_request(void *arg) {
    int sockfd = *(int*)arg;
    free(arg);
    
    Message msg;
    if (receive_message(sockfd, &msg) < 0) {
        close(sockfd);
        return NULL;
    }
    
    Message response;
    init_message(&response);
    
    switch (msg.type) {
        case MSG_READ_FILE: {
            // Check if file is locked for writing
            if (is_file_locked_for_write(msg.filename)) {
                response.type = MSG_ERROR;
                response.error_code = ERR_SENTENCE_LOCKED;
                strcpy(response.data, "File is currently being written");
            } else if (read_file_content(msg.filename, response.data, sizeof(response.data)) == 0) {
                response.type = MSG_RESPONSE;
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
            }
            break;
        }
        
        case MSG_WRITE_FILE: {
            // Check if this is the lock acquisition phase (empty data)
            if (strlen(msg.data) == 0) {
                // Phase 1: Try to acquire file lock
                int file_lock_idx = lock_file_for_write(msg.filename);
                if (file_lock_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SENTENCE_LOCKED;
                    strcpy(response.data, "File is currently being accessed by another user");
                    send_message(sockfd, &response);
                    close(sockfd);
                    return NULL;
                }
                
                // Lock acquired! Send acknowledgment
                response.type = MSG_ACK;
                strcpy(response.data, "LOCK_ACQUIRED");
                send_message(sockfd, &response);
                
                // Now wait for the actual write data
                Message write_msg;
                if (receive_message(sockfd, &write_msg) < 0) {
                    unlock_file_for_write(file_lock_idx);
                    close(sockfd);
                    return NULL;
                }
                
                // Process the actual write with the data
                msg = write_msg;
            } else {
                // This shouldn't happen in the new protocol, but handle it anyway
                int file_lock_idx = lock_file_for_write(msg.filename);
                if (file_lock_idx < 0) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_SENTENCE_LOCKED;
                    strcpy(response.data, "File is currently being accessed by another user");
                    send_message(sockfd, &response);
                    close(sockfd);
                    return NULL;
                }
            }
            
            // Find the lock index for this file
            int file_lock_idx = -1;
            pthread_mutex_lock(&file_lock_list_mutex);
            for (int i = 0; i < file_lock_count; i++) {
                if (strcmp(file_write_locks[i].filename, msg.filename) == 0) {
                    file_lock_idx = i;
                    break;
                }
            }
            pthread_mutex_unlock(&file_lock_list_mutex);
            
            char content[MAX_BUFFER * 4];
            if (read_file_content(msg.filename, content, sizeof(content)) < 0) {
                content[0] = '\0'; // Empty file
            }
            
            // Save for undo
            save_undo(msg.filename, content);
            
            // Split into sentences
            char sentences[1000][MAX_SENTENCE_LEN];
            int num_sentences = 0;
            
            if (strlen(content) > 0) {
                split_into_sentences(content, sentences, &num_sentences);
            }
            
            // Validate sentence index:
            // 1. Index must be >= 0 and <= num_sentences
            // 2. If writing to sentence x where x > 0, sentence x-1 must exist and have proper delimiters
            if (msg.sentence_num < 0 || msg.sentence_num > num_sentences) {
                response.type = MSG_ERROR;
                response.error_code = ERR_INVALID_INDEX;
                strcpy(response.data, "Sentence index out of range");
                if (file_lock_idx >= 0) unlock_file_for_write(file_lock_idx);
                send_message(sockfd, &response);
                close(sockfd);
                return NULL;
            }
            
            // Additional validation: if writing to sentence x where x > 0, 
            // sentence x-1 must exist and end with a delimiter
            if (msg.sentence_num > 0 && msg.sentence_num > num_sentences) {
                response.type = MSG_ERROR;
                response.error_code = ERR_INVALID_INDEX;
                strcpy(response.data, "Sentence index out of range. Previous sentence must exist.");
                if (file_lock_idx >= 0) unlock_file_for_write(file_lock_idx);
                send_message(sockfd, &response);
                close(sockfd);
                return NULL;
            }
            
            // Check if previous sentence has proper delimiter if trying to write beyond current sentences
            if (msg.sentence_num == num_sentences && num_sentences > 0) {
                // Check that the last sentence ends with a delimiter
                int last_sent_idx = num_sentences - 1;
                if (strlen(sentences[last_sent_idx]) == 0 || 
                    (sentences[last_sent_idx][strlen(sentences[last_sent_idx])-1] != '.' &&
                     sentences[last_sent_idx][strlen(sentences[last_sent_idx])-1] != '!' &&
                     sentences[last_sent_idx][strlen(sentences[last_sent_idx])-1] != '?')) {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_INVALID_INDEX;
                    strcpy(response.data, "Sentence index out of range. Previous sentence must be complete with delimiter.");
                    if (file_lock_idx >= 0) unlock_file_for_write(file_lock_idx);
                    send_message(sockfd, &response);
                    close(sockfd);
                    return NULL;
                }
            }
            
            // Initialize or get existing sentence
            char words[MAX_WORDS][MAX_FILENAME];
            int num_words = 0;
            int current_sentence = msg.sentence_num;
            
            if (current_sentence < num_sentences && strlen(sentences[current_sentence]) > 0) {
                split_into_words(sentences[current_sentence], words, &num_words);
            }
            
            // Process write operations from msg.data
            char data_copy[MAX_BUFFER];
            strncpy(data_copy, msg.data, sizeof(data_copy) - 1);
            data_copy[sizeof(data_copy) - 1] = '\0';
            
            char *line = strtok(data_copy, "\n");
            while (line != NULL) {
                if (strcmp(line, "ETIRW") == 0) {
                    break;
                }
                
                int word_idx;
                char word_content[MAX_SENTENCE_LEN];
                
                if (sscanf(line, "%d %[^\n]", &word_idx, word_content) == 2) {
                    // Parse the content and handle delimiters
                    int i = 0;
                    while (word_content[i] && word_content[i] == ' ') i++; // Skip leading spaces
                    
                    while (word_content[i]) {
                        char current_word[MAX_FILENAME] = "";
                        int word_len = 0;
                        int has_delimiter = 0;
                        
                        // Extract one word (including delimiter if present)
                        while (word_content[i] && word_content[i] != ' ') {
                            current_word[word_len++] = word_content[i];
                            // Check if this character is a delimiter
                            if (word_content[i] == '.' || word_content[i] == '!' || word_content[i] == '?') {
                                has_delimiter = 1;
                            }
                            i++;
                        }
                        current_word[word_len] = '\0';
                        
                        if (word_len > 0) {
                            // Validate word index - using 1-based indexing
                            // Valid range: 1 to num_words+1 (inclusive for insertion at end)
                            if (word_idx < 1 || word_idx > num_words + 1) {
                                response.type = MSG_ERROR;
                                response.error_code = ERR_INVALID_INDEX;
                                snprintf(response.data, sizeof(response.data), 
                                         "Word index %d out of range (valid: 1 to %d)", word_idx, num_words + 1);
                                if (file_lock_idx >= 0) unlock_file_for_write(file_lock_idx);
                                send_message(sockfd, &response);
                                close(sockfd);
                                return NULL;
                            }
                            
                            // Convert to 0-based for internal array operations
                            int array_idx = word_idx - 1;
                            
                            // Insert the word at array_idx
                            for (int j = num_words; j > array_idx; j--) {
                                strcpy(words[j], words[j-1]);
                            }
                            strcpy(words[array_idx], current_word);
                            num_words++;
                            word_idx++; // Move to next position for subsequent words
                            
                            // If word contains delimiter, finalize this sentence and start new one
                            if (has_delimiter) {
                                // Reconstruct current sentence
                                reconstruct_sentence(words, num_words, sentences[current_sentence]);
                                
                                // Move remaining sentences down if we're inserting in middle
                                if (current_sentence < num_sentences - 1) {
                                    for (int j = num_sentences; j > current_sentence + 1; j--) {
                                        strcpy(sentences[j], sentences[j-1]);
                                    }
                                }
                                
                                // Move to next sentence index
                                current_sentence++;
                                num_sentences = (current_sentence >= num_sentences) ? current_sentence + 1 : num_sentences + 1;
                                
                                // Initialize new sentence (empty for now, will be filled if there are more words)
                                num_words = 0;
                                word_idx = 1; // Reset to 1 for 1-based indexing
                                sentences[current_sentence][0] = '\0';
                            }
                        }
                        
                        // Skip spaces
                        while (word_content[i] && word_content[i] == ' ') i++;
                    }
                }
                
                line = strtok(NULL, "\n");
            }
            
            // Reconstruct final sentence if there are remaining words
            if (num_words > 0) {
                if (current_sentence >= num_sentences) {
                    num_sentences = current_sentence + 1;
                }
                reconstruct_sentence(words, num_words, sentences[current_sentence]);
            }
            
            // Reconstruct file content
            content[0] = '\0';
            for (int i = 0; i < num_sentences; i++) {
                if (strlen(sentences[i]) > 0) {
                    strcat(content, sentences[i]);
                    if (i < num_sentences - 1 && strlen(sentences[i+1]) > 0) {
                        strcat(content, " ");
                    }
                }
            }
            
            write_file_content(msg.filename, content);
            
            // Unlock file
            if (file_lock_idx >= 0) unlock_file_for_write(file_lock_idx);
            
            response.type = MSG_ACK;
            break;
        }

        case MSG_STREAM_FILE: {
            // Check if file is locked for writing
            if (is_file_locked_for_write(msg.filename)) {
                response.type = MSG_ERROR;
                response.error_code = ERR_SENTENCE_LOCKED;
                strcpy(response.data, "File is currently being written");
                send_message(sockfd, &response);
                break;
            }
            
            char content[MAX_BUFFER * 4];
            if (read_file_content(msg.filename, content, sizeof(content)) < 0) {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
                send_message(sockfd, &response);
                break;
            }
            
            // Stream word by word
            char *word = strtok(content, " \n");
            while (word) {
                init_message(&response);
                response.type = MSG_RESPONSE;
                strcpy(response.data, word);
                send_message(sockfd, &response);
                usleep(100000); // 0.1 second delay
                word = strtok(NULL, " \n");
            }
            
            // Send STOP signal
            init_message(&response);
            response.type = MSG_ACK;
            strcpy(response.data, "STOP");
            send_message(sockfd, &response);
            close(sockfd);
            return NULL;
        }
        
        case MSG_UNDO: {
            pthread_mutex_lock(&undo_lock);
            int found = -1;
            for (int i = undo_count - 1; i >= 0; i--) {
                int idx = i % MAX_FILES;
                if (strcmp(undo_history[idx].filename, msg.filename) == 0) {
                    found = idx;
                    break;
                }
            }
            
            if (found >= 0) {
                write_file_content(msg.filename, undo_history[found].content);
                response.type = MSG_ACK;
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
            }
            pthread_mutex_unlock(&undo_lock);
            break;
        }
        
        case MSG_INFO_FILE: {
            struct stat st;
            char filepath[MAX_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, msg.filename);
            
            if (stat(filepath, &st) == 0) {
                char content[MAX_BUFFER];
                read_file_content(msg.filename, content, sizeof(content));
                
                int word_count = 0;
                for (int i = 0; content[i]; i++) {
                    if (content[i] == ' ' || content[i] == '\n') word_count++;
                }
                if (content[0]) word_count++;
                
                snprintf(response.data, sizeof(response.data),
                         "Size: %ld bytes\nWords: %d\nChars: %ld\nModified: %s",
                         st.st_size, word_count, st.st_size, ctime(&st.st_mtime));
                response.type = MSG_RESPONSE;
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
            }
            break;
        }
        
        // ===== FOLDER OPERATIONS =====
        case MSG_CREATE_FOLDER: {
            if (create_folder(msg.folder_path) == 0) {
                response.type = MSG_ACK;
                snprintf(response.data, sizeof(response.data), "Folder '%s' created successfully", msg.folder_path);
                log_message("SS", "Folder created");
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_INVALID_COMMAND;
                snprintf(response.data, sizeof(response.data), "Failed to create folder '%s'", msg.folder_path);
            }
            break;
        }
        
        case MSG_MOVE_FILE: {
            // msg.filename contains the file to move
            // msg.folder_path contains the destination folder
            if (move_file_to_folder(msg.filename, msg.folder_path) == 0) {
                response.type = MSG_ACK;
                snprintf(response.data, sizeof(response.data), "File '%s' moved to folder '%s' successfully", 
                         msg.filename, msg.folder_path);
                log_message("SS", "File moved to folder");
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_INVALID_COMMAND;
                snprintf(response.data, sizeof(response.data), "Failed to move file '%s' to folder '%s'", 
                         msg.filename, msg.folder_path);
            }
            break;
        }
        
        case MSG_VIEW_FOLDER: {
            // msg.folder_path contains the folder to view
            if (list_folder_contents(msg.folder_path, response.data, sizeof(response.data)) == 0) {
                response.type = MSG_RESPONSE;
                log_message("SS", "Folder contents listed");
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, sizeof(response.data), "Folder '%s' not found", msg.folder_path);
            }
            break;
        }
        // ===== END FOLDER OPERATIONS =====
        
        // ===== CHECKPOINT OPERATIONS =====
        case MSG_CHECKPOINT: {
            // msg.filename contains the filename
            // msg.data contains the tag
            // Read current file content first
            char current_content[MAX_BUFFER * 4];
            if (read_file_content(msg.filename, current_content, sizeof(current_content)) == 0) {
                // Create checkpoint
                if (create_checkpoint(msg.filename, msg.data, current_content, msg.username) == 0) {
                    response.type = MSG_ACK;
                    snprintf(response.data, sizeof(response.data), "Checkpoint '%s' created successfully", msg.data);
                    log_message("SS", "Checkpoint created");
                } else {
                    response.type = MSG_ERROR;
                    response.error_code = ERR_INVALID_COMMAND;
                    snprintf(response.data, sizeof(response.data), "Failed to create checkpoint '%s' (may already exist)", msg.data);
                }
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, sizeof(response.data), "File '%s' not found", msg.filename);
            }
            break;
        }
        
        case MSG_VIEWCHECKPOINT: {
            // msg.filename contains the filename
            // msg.data contains the tag
            char checkpoint_content[MAX_BUFFER * 4];
            if (view_checkpoint(msg.filename, msg.data, checkpoint_content, sizeof(checkpoint_content)) == 0) {
                response.type = MSG_RESPONSE;
                strncpy(response.data, checkpoint_content, sizeof(response.data) - 1);
                response.data[sizeof(response.data) - 1] = '\0';
                log_message("SS", "Checkpoint viewed");
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, sizeof(response.data), "Checkpoint '%s' not found for file '%s'", msg.data, msg.filename);
            }
            break;
        }
        
        case MSG_REVERT: {
            // msg.filename contains the filename
            // msg.data contains the tag
            if (revert_checkpoint(msg.filename, msg.data) == 0) {
                response.type = MSG_ACK;
                snprintf(response.data, sizeof(response.data), "File reverted to checkpoint '%s'", msg.data);
                log_message("SS", "File reverted to checkpoint");
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, sizeof(response.data), "Checkpoint '%s' not found for file '%s'", msg.data, msg.filename);
            }
            break;
        }
        
        case MSG_LISTCHECKPOINTS: {
            // msg.filename contains the filename
            if (list_checkpoints(msg.filename, response.data, sizeof(response.data)) == 0) {
                response.type = MSG_RESPONSE;
                log_message("SS", "Checkpoints listed");
            } else {
                response.type = MSG_ERROR;
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, sizeof(response.data), "No checkpoints found for file '%s'", msg.filename);
            }
            break;
        }
        // ===== END CHECKPOINT OPERATIONS =====
    }
    
    send_message(sockfd, &response);
    close(sockfd);
    return NULL;
}

void *nm_listener_thread(void *arg) {
    int server_fd = *(int*)arg;
    
    while (1) {
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_fd, NULL, NULL);
        
        if (*client_sock < 0) {
            free(client_sock);
            continue;
        }
        
        pthread_t tid;
        pthread_create(&tid, NULL, handle_nm_request, client_sock);
        pthread_detach(tid);
    }
    
    return NULL;
}

void *client_listener_thread(void *arg) {
    int server_fd = *(int*)arg;
    
    while (1) {
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_fd, NULL, NULL);
        
        if (*client_sock < 0) {
            free(client_sock);
            continue;
        }
        
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client_request, client_sock);
        pthread_detach(tid);
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <nm_ip> <nm_port> <ss_port> <storage_dir>\n", argv[0]);
        return 1;
    }
    
    char *nm_ip = argv[1];
    int nm_port = atoi(argv[2]);
    int ss_port = atoi(argv[3]);
    strcpy(storage_dir, argv[4]);
    
    nm_port_listen = ss_port;
    client_port_listen = ss_port + 1;
    
    init_storage();
    
    // Register with Naming Server
    int nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(nm_port);
    inet_pton(AF_INET, nm_ip, &nm_addr.sin_addr);
    
    if (connect(nm_sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Failed to connect to Naming Server");
        return 1;
    }
    
    Message reg_msg;
    init_message(&reg_msg);
    reg_msg.type = MSG_REGISTER_SS;
    gethostname(reg_msg.ss_ip, sizeof(reg_msg.ss_ip));
    struct in_addr addr;
    inet_pton(AF_INET, "127.0.0.1", &addr);
    inet_ntop(AF_INET, &addr, reg_msg.ss_ip, sizeof(reg_msg.ss_ip));
    reg_msg.ss_port = nm_port_listen;
    reg_msg.flags = client_port_listen;
    
    // List files in storage directory
    DIR *dir = opendir(storage_dir);
    if (dir) {
        struct dirent *ent;
        reg_msg.data[0] = '\0';
        while ((ent = readdir(dir))) {
            if (ent->d_type == DT_REG) {
                strcat(reg_msg.data, ent->d_name);
                strcat(reg_msg.data, "\n");
            }
        }
        closedir(dir);
    }
    
    send_message(nm_sock, &reg_msg);
    
    Message ack;
    receive_message(nm_sock, &ack);
    close(nm_sock);
    
    if (ack.type != MSG_ACK) {
        fprintf(stderr, "Registration failed\n");
        return 1;
    }
    
    log_message("SS", "Registered with Naming Server");
    
    // Create two server sockets - one for NM, one for clients
    int nm_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int client_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(nm_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(client_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in nm_listen_addr, client_listen_addr;
    
    // NM listener
    nm_listen_addr.sin_family = AF_INET;
    nm_listen_addr.sin_addr.s_addr = INADDR_ANY;
    nm_listen_addr.sin_port = htons(nm_port_listen);
    bind(nm_server_fd, (struct sockaddr*)&nm_listen_addr, sizeof(nm_listen_addr));
    listen(nm_server_fd, 50);
    
    // Client listener
    client_listen_addr.sin_family = AF_INET;
    client_listen_addr.sin_addr.s_addr = INADDR_ANY;
    client_listen_addr.sin_port = htons(client_port_listen);
    bind(client_server_fd, (struct sockaddr*)&client_listen_addr, sizeof(client_listen_addr));
    listen(client_server_fd, 50);
    
    printf("Storage Server listening on:\n");
    printf("  NM port: %d\n", nm_port_listen);
    printf("  Client port: %d\n", client_port_listen);
    
    // Start listener threads
    pthread_t nm_thread, client_thread;
    pthread_create(&nm_thread, NULL, nm_listener_thread, &nm_server_fd);
    pthread_create(&client_thread, NULL, client_listener_thread, &client_server_fd);
    
    pthread_join(nm_thread, NULL);
    pthread_join(client_thread, NULL);
    
    return 0;
}