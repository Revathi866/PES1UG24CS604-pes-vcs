#include "index.h"
#include "tree.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    if (!index || !path) return NULL;
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:      %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0 || strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            if (!index_find((Index *)index, ent->d_name)) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int index_load(Index *index) {
    // CRITICAL: Initialize memory to zero to prevent Segfaults from garbage count
    memset(index, 0, sizeof(Index));
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f) && index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 1];
        if (sscanf(line, "%o %64s %" SCNd64 " %u %511[^\n]", 
                   &e->mode, hex, &e->mtime_sec, &e->size, e->path) == 5) {
            hex_to_hash(hex, &e->hash);
            index->count++;
        }
    }
    fclose(f);
    return 0;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    Index sorted_index = *index;
    qsort(sorted_index.entries, sorted_index.count, sizeof(IndexEntry), compare_index_entries);
    
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < sorted_index.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted_index.entries[i].hash, hex);
        fprintf(f, "%o %s %" PRId64 " %u %s\n", 
                sorted_index.entries[i].mode, hex, 
                sorted_index.entries[i].mtime_sec, 
                sorted_index.entries[i].size, sorted_index.entries[i].path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    // Read and write the blob
    ObjectID blob_id;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *data = malloc(st.st_size + 1);
    if (!data) { fclose(f); return -1; }
    
    if (st.st_size > 0) {
        size_t rs = fread(data, 1, st.st_size, f);
        (void)rs;
    }
    fclose(f);

    if (object_write(OBJ_BLOB, data, st.st_size, &blob_id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    // Update index entry
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
    }

    e->mode = get_file_mode(path);
    e->hash = blob_id;
    e->mtime_sec = (int64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;
    memset(e->path, 0, sizeof(e->path));
    strncpy(e->path, path, sizeof(e->path) - 1);

    return index_save(index);
}
