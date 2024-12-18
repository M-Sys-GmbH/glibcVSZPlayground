#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 50
#define MAX_MAPPING_PATH 1024
#define MAX_ADDRESS_LEN  64

struct pmap_entry {
    unsigned long long start_address;
    int kbytes;
    int rss;
    int dirty;
    char r;
    char w;
    char x;
    char mapping[MAX_MAPPING_PATH];
};

FILE *execute_pmap_cmd(int pid) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pmap -x %d", pid);

    FILE *fp = popen(cmd, "r");
    if (fp == NULL) {
        perror("popen");
        return NULL;
    }
    return fp;
}

static struct pmap_entry *resize_entries(struct pmap_entry *entries, int *capacity) {
    *capacity *= 2;
    struct pmap_entry *new_entries = realloc(entries, sizeof(struct pmap_entry) * (*capacity));
    if (!new_entries) {
        perror("realloc");
        free(entries);
        return NULL;
    }
    return new_entries;
}

static struct pmap_entry *parse_pmap_output(FILE *fp, int *count) {
    int capacity = INITIAL_CAPACITY;
    struct pmap_entry *entries = malloc(sizeof(struct pmap_entry) * capacity);
    if (!entries) {
        fprintf(stderr, "Malloc pmap entries failed!\n");
        return NULL;
    }

    char line[1024];
    *count = 0;

    // Skip header line
    if (fgets(line, sizeof(line), fp) == NULL) {
        fprintf(stderr, "No output from pmap.\n");
        free(entries);
        return NULL;
    }

    // Parse each subsequent line
    while (fgets(line, sizeof(line), fp) != NULL) {
        // If we are out of space, realloc
        if (*count >= capacity) {
            entries = resize_entries(entries, &capacity);
            if (!entries) {
                return NULL;
            }
        }

        char address[MAX_ADDRESS_LEN];
        int kbytes, rss, dirty;
        char mode[8];
        char mapping[MAX_MAPPING_PATH];
        mapping[0] = '\0';

        int fields = sscanf(line, "%63s %d %d %d %7s %1023[^\n]",
                            address, &kbytes, &rss, &dirty, mode, mapping);
        if (fields < 5) {
            continue; // skip malformed lines
        }

        // Convert address
        unsigned long long start_address = strtoull(address, NULL, 16);

        // Break down mode
        char r = (mode[0] == 'r') ? 'r' : '-';
        char w = (mode[1] == 'w') ? 'w' : '-';
        char x = (mode[2] == 'x') ? 'x' : '-';

        // Fill in the struct
        entries[*count].start_address = start_address;
        entries[*count].kbytes = kbytes;
        entries[*count].rss = rss;
        entries[*count].dirty = dirty;
        entries[*count].r = r;
        entries[*count].w = w;
        entries[*count].x = x;
        if (fields == 6) {
            // Trim leading spaces from mapping if any
            char *m = mapping;
            while (*m == ' ') m++;
            strncpy(entries[*count].mapping, m, MAX_MAPPING_PATH);
            entries[*count].mapping[MAX_MAPPING_PATH - 1] = '\0';
        } else {
            entries[*count].mapping[0] = '\0';
        }

        (*count)++;
    }
    return entries;
}

void print_pmap_entries(struct pmap_entry *entries, int count) {
    printf("%-12s %8s %8s %8s %s%s%s %s\n",
           "Address", "Kbytes", "RSS", "Dirty", "R", "W", "X", "Mapping");
    for (int i = 0; i < count; i++) {
        printf("%llx %8d %8d %8d %c%c%c %s\n",
               entries[i].start_address,
               entries[i].kbytes,
               entries[i].rss,
               entries[i].dirty,
               entries[i].r,
               entries[i].w,
               entries[i].x,
               entries[i].mapping);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <pid>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int pid = atoi(argv[1]);

    FILE *fp = execute_pmap_cmd(pid);
    if (!fp) {
        return EXIT_FAILURE;
    }

    int count = 0;
    struct pmap_entry *entries = parse_pmap_output(fp, &count);

    pclose(fp);

    if (!entries) {
        return EXIT_FAILURE;
    }

    print_pmap_entries(entries, count);

    free(entries);

    return EXIT_SUCCESS;
}

