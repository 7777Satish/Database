#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

// --- CONSTANTS & CONFIGURATION ---

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

#define PAGE_SIZE 4096
#define PAGE_HEADER_SIZE 16
#define PAGE_PAYLOAD_SIZE (PAGE_SIZE - PAGE_HEADER_SIZE)

#define POOL_CAPACITY 32      // As demonstrated in the PPT
#define HASH_BUCKETS 64       // For the buffer pool's chained hash table

#define MAGIC_NUMBER 0x4D696E69 // "Mini"
#define DB_VERSION 1

// Page Types
typedef enum {
    PAGE_TYPE_HEADER = 0,
    PAGE_TYPE_DATA = 1,
    PAGE_TYPE_FREE = 2
} PageType;

// --- ROW STRUCTURE ---

typedef struct {
    uint32_t id;
    char username[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
} Row;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct *)0)->Attribute)
const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);

const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

// Because pages have a 16-byte header, rows must fit in the remaining 4080 bytes
const uint32_t ROWS_PER_PAGE = PAGE_PAYLOAD_SIZE / ROW_SIZE;

// --- BUFFER POOL STRUCTURES ---

/* 
 * Represents a 4KB page in memory.
 * Includes pointers to manage its place in the Hash Table, LRU list, and Free list.
 */
typedef struct Frame {
    uint32_t frame_id;          // Internal array index (0 to 31)
    uint32_t page_id;           // The database page number it currently holds
    uint8_t data[PAGE_SIZE];    // The actual 4KB block
    
    bool is_dirty;              // Needs write-back to disk?
    uint32_t pin_count;         // How many threads/queries are actively using this
    
    struct Frame* hash_next;    // For collision chaining in the Hash Table
    struct Frame* lru_prev;     // Doubly-linked list pointers for LRU eviction
    struct Frame* lru_next;
    struct Frame* free_next;    // Pointer for the free-frame list
} Frame;

typedef struct {
    int file_descriptor;
    uint32_t page_count;        // Tracked in the header (Page 0)
    uint32_t free_list_head;    // Unused pages for reuse
    uint32_t num_rows;          // Maintained in header for the flat-table abstraction
    
    Frame frames[POOL_CAPACITY];
    
    // Cache management lists
    Frame* hash_table[HASH_BUCKETS];
    Frame* free_frames_head;
    
    // LRU Doubly-linked list (tracks unpinned pages)
    Frame* lru_head; // Most recently used (unpinned)
    Frame* lru_tail; // Least recently used (candidate for eviction)
    
    // Stats for benchmarking
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t writebacks;
} BufferPool;

typedef struct {
    BufferPool* pool;
} Table;

// --- CRC32 IMPLEMENTATION ---

// Simple bitwise CRC32 calculation (Standard IEEE 802.3)
// Checksums ONLY the payload (bytes 16 to 4095) as per the PPT
uint32_t calculate_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

// --- LRU & HASH TABLE HELPERS ---

// Hash function: simple modulo
uint32_t hash_page(uint32_t page_id) {
    return page_id % HASH_BUCKETS;
}

void lru_remove(BufferPool* pool, Frame* frame) {
    if (frame->lru_prev) frame->lru_prev->lru_next = frame->lru_next;
    else pool->lru_head = frame->lru_next;

    if (frame->lru_next) frame->lru_next->lru_prev = frame->lru_prev;
    else pool->lru_tail = frame->lru_prev;

    frame->lru_prev = NULL;
    frame->lru_next = NULL;
}

void lru_push_front(BufferPool* pool, Frame* frame) {
    frame->lru_prev = NULL;
    frame->lru_next = pool->lru_head;
    if (pool->lru_head) pool->lru_head->lru_prev = frame;
    pool->lru_head = frame;
    if (!pool->lru_tail) pool->lru_tail = frame;
}

void hash_insert(BufferPool* pool, Frame* frame) {
    uint32_t bucket = hash_page(frame->page_id);
    frame->hash_next = pool->hash_table[bucket];
    pool->hash_table[bucket] = frame;
}

void hash_remove(BufferPool* pool, Frame* frame) {
    uint32_t bucket = hash_page(frame->page_id);
    Frame* curr = pool->hash_table[bucket];
    Frame* prev = NULL;
    
    while (curr) {
        if (curr->frame_id == frame->frame_id) {
            if (prev) prev->hash_next = curr->hash_next;
            else pool->hash_table[bucket] = curr->hash_next;
            curr->hash_next = NULL;
            return;
        }
        prev = curr;
        curr = curr->hash_next;
    }
}

// --- DISK I/O (THE PAGER) ---

void write_page_to_disk(BufferPool* pool, Frame* frame) {
    // 1. Pack the 16-byte header
    uint32_t* header = (uint32_t*)frame->data;
    header[0] = frame->page_id;
    // (Type is managed elsewhere, assume already set in header[1])
    
    // 2. Calculate and set the CRC32 of the payload
    uint32_t payload_crc = calculate_crc32(frame->data + PAGE_HEADER_SIZE, PAGE_PAYLOAD_SIZE);
    header[2] = payload_crc; 
    
    // 3. Write out to POSIX fd
    off_t offset = lseek(pool->file_descriptor, frame->page_id * PAGE_SIZE, SEEK_SET);
    if (offset == -1) {
        printf("Error: Failed to seek to page %u\n", frame->page_id);
        exit(EXIT_FAILURE);
    }
    
    if (write(pool->file_descriptor, frame->data, PAGE_SIZE) != PAGE_SIZE) {
        printf("Error: Failed to write page %u\n", frame->page_id);
        exit(EXIT_FAILURE);
    }
    pool->writebacks++;
}

void read_page_from_disk(BufferPool* pool, Frame* frame, uint32_t page_id) {
    frame->page_id = page_id;
    off_t offset = lseek(pool->file_descriptor, page_id * PAGE_SIZE, SEEK_SET);
    
    ssize_t bytes_read = read(pool->file_descriptor, frame->data, PAGE_SIZE);
    
    // If the page doesn't exist yet, zero it out and format the header
    if (bytes_read <= 0) {
        memset(frame->data, 0, PAGE_SIZE);
        uint32_t* header = (uint32_t*)frame->data;
        header[0] = page_id;
        header[1] = PAGE_TYPE_DATA;
        return;
    }
    
    // Checksum verification
    uint32_t* header = (uint32_t*)frame->data;
    uint32_t stored_crc = header[2];
    uint32_t calculated_crc = calculate_crc32(frame->data + PAGE_HEADER_SIZE, PAGE_PAYLOAD_SIZE);
    
    // Bypass CRC check for page 0 (Header page uses different layout)
    if (page_id != 0 && stored_crc != calculated_crc) {
        printf("CRITICAL CORRUPTION DETECTED: Page %u CRC mismatch. Expected %x, got %x\n", 
               page_id, stored_crc, calculated_crc);
        exit(EXIT_FAILURE);
    }
}

// --- BUFFER POOL PUBLIC API ---

BufferPool* pool_open(const char* filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd == -1) {
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    BufferPool* pool = malloc(sizeof(BufferPool));
    memset(pool, 0, sizeof(BufferPool));
    pool->file_descriptor = fd;
    
    // Initialize the free frames list
    for (int i = POOL_CAPACITY - 1; i >= 0; i--) {
        pool->frames[i].frame_id = i;
        pool->frames[i].free_next = pool->free_frames_head;
        pool->free_frames_head = &pool->frames[i];
    }
    
    // Handle Page 0 (Database Header)
    off_t file_length = lseek(fd, 0, SEEK_END);
    if (file_length == 0) {
        // Brand new database file: initialize metadata
        pool->page_count = 1; // Includes page 0
        pool->free_list_head = 0;
        pool->num_rows = 0;
    } else {
        // Read existing header
        uint8_t header_buf[PAGE_SIZE];
        lseek(fd, 0, SEEK_SET);
        read(fd, header_buf, PAGE_SIZE);
        
        uint32_t* meta = (uint32_t*)header_buf;
        if (meta[0] != MAGIC_NUMBER) {
            printf("Error: File is not a valid MiniDB database.\n");
            exit(EXIT_FAILURE);
        }
        pool->page_count = meta[2];
        pool->free_list_head = meta[3];
        pool->num_rows = meta[4];
    }

    return pool;
}

// Fetches a page, pins it in memory, and returns a pointer to its start.
void* get_page(BufferPool* pool, uint32_t page_id) {
    // 1. Check Hash Table (Cache Hit)
    uint32_t bucket = hash_page(page_id);
    Frame* curr = pool->hash_table[bucket];
    while (curr) {
        if (curr->page_id == page_id) {
            pool->hits++;
            // If it was unpinned, remove it from LRU since we are pinning it now
            if (curr->pin_count == 0) {
                lru_remove(pool, curr);
            }
            curr->pin_count++;
            return curr->data;
        }
        curr = curr->hash_next;
    }

    // 2. Cache Miss: We need a frame
    pool->misses++;
    Frame* target_frame = NULL;

    if (pool->free_frames_head != NULL) {
        // Use a free frame
        target_frame = pool->free_frames_head;
        pool->free_frames_head = target_frame->free_next;
    } else {
        // Pool is full: Evict the Least Recently Used frame
        target_frame = pool->lru_tail;
        if (target_frame == NULL) {
            printf("Error: Buffer pool exhausted. All frames are pinned.\n");
            exit(EXIT_FAILURE);
        }
        
        pool->evictions++;
        lru_remove(pool, target_frame);
        hash_remove(pool, target_frame);
        
        // Dirty Write-Back
        if (target_frame->is_dirty) {
            write_page_to_disk(pool, target_frame);
            target_frame->is_dirty = false;
        }
    }

    // 3. Load the new page from disk
    read_page_from_disk(pool, target_frame, page_id);
    hash_insert(pool, target_frame);
    
    target_frame->pin_count = 1; // Pin it
    return target_frame->data;
}

// Unpins a page so it can be evicted if necessary.
void unpin_page(BufferPool* pool, uint32_t page_id, bool is_dirty) {
    uint32_t bucket = hash_page(page_id);
    Frame* curr = pool->hash_table[bucket];
    while (curr) {
        if (curr->page_id == page_id) {
            if (is_dirty) curr->is_dirty = true;
            if (curr->pin_count > 0) curr->pin_count--;
            
            // If nobody is using it, push it to the front of the LRU
            if (curr->pin_count == 0) {
                lru_push_front(pool, curr);
            }
            return;
        }
        curr = curr->hash_next;
    }
}

void db_close(Table* table) {
    BufferPool* pool = table->pool;

    // 1. Flush all dirty frames
    for (int i = 0; i < POOL_CAPACITY; i++) {
        if (pool->frames[i].pin_count > 0) {
            printf("Warning: Frame %d is still pinned during shutdown.\n", i);
        }
        if (pool->frames[i].is_dirty) {
            write_page_to_disk(pool, &pool->frames[i]);
        }
    }

    // 2. Write the Database Header (Page 0) to save state
    uint8_t header_buf[PAGE_SIZE] = {0};
    uint32_t* meta = (uint32_t*)header_buf;
    meta[0] = MAGIC_NUMBER;
    meta[1] = DB_VERSION;
    meta[2] = pool->page_count;
    meta[3] = pool->free_list_head;
    meta[4] = pool->num_rows;
    
    lseek(pool->file_descriptor, 0, SEEK_SET);
    write(pool->file_descriptor, header_buf, PAGE_SIZE);

    close(pool->file_descriptor);
    free(pool);
    free(table);
}

// --- ROW SERIALIZATION & MAPPING ---

void serialize_row(Row *source, void *destination) {
    memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE);
    memcpy(destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy(destination + EMAIL_OFFSET, &(source->email), EMAIL_SIZE);
}

void deserialize_row(void *source, Row *destination) {
    memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE);
    memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

// Retrieves the memory address to read/write a specific row.
// Note: Row data starts at Page 1. Page 0 is the Database Header.
void* row_slot(Table* table, uint32_t row_num, uint32_t* out_page_id) {
    uint32_t page_offset = row_num / ROWS_PER_PAGE;
    uint32_t page_id = page_offset + 1; // +1 because Page 0 is header
    
    // If we require a page ID higher than we've recorded, update file metadata
    if (page_id >= table->pool->page_count) {
        table->pool->page_count = page_id + 1;
    }
    
    void* page_data = get_page(table->pool, page_id);
    
    uint32_t row_offset = row_num % ROWS_PER_PAGE;
    uint32_t byte_offset = PAGE_HEADER_SIZE + (row_offset * ROW_SIZE);
    
    *out_page_id = page_id;
    return (uint8_t*)page_data + byte_offset;
}

// --- REPL & PARSER ---

typedef struct {
    char *buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

InputBuffer *new_input_buffer() {
    InputBuffer *input_buffer = malloc(sizeof(InputBuffer));
    input_buffer->buffer = NULL;
    input_buffer->buffer_length = 0;
    input_buffer->input_length = 0;
    return input_buffer;
}

void print_prompt() { printf("minidb > "); }

void read_input(InputBuffer *input_buffer) {
    ssize_t bytes_read = getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);
    if (bytes_read <= 0) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }
    // Ignore trailing newline
    input_buffer->input_length = bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = 0;
}

void parse_sql(InputBuffer *input, Table *table) {
    if (!strncasecmp(input->buffer, "select", 6)) {
        for (uint32_t i = 0; i < table->pool->num_rows; i++) {
            Row row;
            uint32_t page_id;
            
            // This fetches and implicitly PINS the page
            void *destination = row_slot(table, i, &page_id);
            deserialize_row(destination, &row);
            printf("(%u, %s, %s)\n", row.id, row.username, row.email);
            
            // UNPIN it so it can be evicted by LRU if memory fills up
            unpin_page(table->pool, page_id, false);
        }
        printf("EXECUTED\n");
        
    } else if (!strncasecmp(input->buffer, "insert", 6)) {
        Row row;
        int args_read = sscanf(input->buffer, "insert %u %32s %255s", &(row.id), row.username, row.email);
        
        if (args_read < 3) {
            printf("Error: Syntax error. Use 'insert <id> <username> <email>'\n");
            return;
        }

        uint32_t page_id;
        
        // This fetches and implicitly PINS the page
        void *destination = row_slot(table, table->pool->num_rows, &page_id);
        serialize_row(&row, destination);
        
        table->pool->num_rows++;
        
        // UNPIN it and mark it DIRTY so the write-back mechanism saves it later
        unpin_page(table->pool, page_id, true);
        printf("EXECUTED\n");
        
    } else if (!strncasecmp(input->buffer, ".stats", 6)) {
        // Added command to verify caching as shown in presentation
        BufferPool* p = table->pool;
        printf("capacity=%d resident_pages=%d hits=%u misses=%u evictions=%u writebacks=%u\n",
                POOL_CAPACITY, p->page_count, p->hits, p->misses, p->evictions, p->writebacks);
    } else if (!strncasecmp(input->buffer, ".exit", 5)) {
        db_close(table);
        exit(EXIT_SUCCESS);
    } else {
        printf("Unrecognized command '%s'.\n", input->buffer);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }
    
    Table* table = malloc(sizeof(Table));
    table->pool = pool_open(argv[1]);
    InputBuffer *ibuffer = new_input_buffer();

    while (1) {
        print_prompt();
        read_input(ibuffer);
        if (ibuffer->input_length == 0) continue;
        parse_sql(ibuffer, table);
    }

    return 0;
}