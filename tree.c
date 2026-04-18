// tree.c — Tree object serialization and construction
#include "pes.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────
#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

static int write_tree_recursive(const Index *idx, int start, int end, int depth, ObjectID *id_out) {
    Tree *tree = malloc(sizeof(Tree));
    if (!tree) return -1;
    tree->count = 0;

    for (int i = start; i < end; ) {
        const char *path = idx->entries[i].path;
        const char *relative_path = path;
        
        // Skip path components based on current depth
        for(int d = 0; d < depth; d++) {
            const char *slash_ptr = strchr(relative_path, '/');
            if (slash_ptr) relative_path = slash_ptr + 1;
        }

        const char *slash = strchr(relative_path, '/');
        if (tree->count >= MAX_TREE_ENTRIES) { free(tree); return -1; }
        TreeEntry *entry = &tree->entries[tree->count++];
        
        if (slash) {
            // Entry is a subdirectory
            size_t dir_name_len = slash - relative_path;
            strncpy(entry->name, relative_path, dir_name_len);
            entry->name[dir_name_len] = '\0';
            entry->mode = MODE_DIR;

            int next_group = i;
            size_t prefix_len = slash - path + 1;
            while (next_group < end && strncmp(idx->entries[next_group].path, path, prefix_len) == 0) {
                next_group++;
            }
            
            if (write_tree_recursive(idx, i, next_group, depth + 1, &entry->hash) != 0) {
                free(tree);
                return -1;
            }
            i = next_group; 
        } else {
            // Entry is a file
            strncpy(entry->name, relative_path, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            entry->mode = idx->entries[i].mode;
            entry->hash = idx->entries[i].hash;
            i++;
        }
    }

    void *buffer = NULL;
    size_t len = 0;
    if (tree_serialize(tree, &buffer, &len) != 0) { free(tree); return -1; }

    if (object_write(OBJ_TREE, buffer, len, id_out) != 0) {
        free(buffer);
        free(tree);
        return -1;
    }

    free(buffer);
    free(tree);
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    // Allocation on the HEAP to prevent Segfault
    Index *idx = malloc(sizeof(Index));
    if (!idx) return -1;

    // Load staged files from the index
    if (index_load(idx) != 0) {
        free(idx);
        return -1;
    }
    
    if (idx->count == 0) {
        free(idx);
        return -1; 
    }
    
    // Pass the pointer to the recursive function
    int result = write_tree_recursive(idx, 0, idx->count, 0, id_out);
    
    free(idx); // Clean up
    return result;
}
