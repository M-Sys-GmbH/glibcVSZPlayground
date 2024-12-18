#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 5
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

int resize_entries(struct pmap_entry **entries, int *capacity) {
    (*capacity) *= 2;
    struct pmap_entry *new_entries = realloc(*entries, sizeof(struct pmap_entry) * (*capacity));
    if (!new_entries) {
        perror("realloc");
        free(*entries);
        return -1;
    }
    *entries = new_entries;

    return 0;
}

int parse_pmap_output(FILE **fp, struct pmap_entry **entries, int *count) {
    int capacity = INITIAL_CAPACITY;

    char line[1024];

    // Skip header line
    if (fgets(line, sizeof(line), *fp) == NULL) {
        fprintf(stderr, "No output from pmap.\n");
        free(*entries);
        pclose(*fp);
        return -1;
    }

    // Parse each subsequent line
    while (fgets(line, sizeof(line), *fp) != NULL) {
        // If we are out of space, realloc
        if (*count >= capacity) {
            if (resize_entries(entries, &capacity) != 0) {
                free(*fp);
                return -1;
            }
        }

        // Temp fields to parse
        char address[MAX_ADDRESS_LEN];
        int kbytes, rss, dirty;
        char mode[8];
        char mapping[MAX_MAPPING_PATH];

        // Initialize mapping to empty to handle no mapping scenario
        mapping[0] = '\0';

        // The format of each line (after header) is typically:
        // Address           Kbytes     RSS   Dirty Mode  Mapping
        // Attempt to parse:
        int fields = sscanf(line, "%63s %d %d %d %7s %1023[^\n]",
                            address, &kbytes, &rss, &dirty, mode, mapping);
        unsigned long long start_address = strtoull(address, NULL, 16);
        // Must have at least Address, Kbytes, RSS, Dirty, and Mode
        if (fields < 5) {
            continue; // skip malformed lines
        }

        // Break down mode
        char r = (mode[0] == 'r') ? 'r' : '-';
        char w = (mode[1] == 'w') ? 'w' : '-';
        char x = (mode[2] == 'x') ? 'x' : '-';

        // Fill in the struct
        (*entries)[*count].start_address = start_address;
        (*entries)[*count].kbytes = kbytes;
        (*entries)[*count].rss = rss;
        (*entries)[*count].dirty = dirty;
        (*entries)[*count].r = r;
        (*entries)[*count].w = w;
        (*entries)[*count].x = x;
        if (fields == 6) {
            // We got a mapping
            // Trim leading spaces from mapping if any
            char *m = mapping;
            while (*m == ' ') m++;
            strncpy((*entries)[*count].mapping, m, MAX_MAPPING_PATH);
            (*entries)[*count].mapping[MAX_MAPPING_PATH - 1] = '\0';
        } else {
            (*entries)[*count].mapping[0] = '\0';
        }

        (*count)++;
    }
    return 0;
}

void print_pmap_entries(struct pmap_entry **entries, int count) {
    printf("%-12s %8s %8s %8s %s%s%s %s\n",
           "Address", "Kbytes", "RSS", "Dirty", "R", "W", "X", "Mapping");
    for (int i = 0; i < count; i++) {
        printf("%llx %8d %8d %8d %c%c%c %s\n",
               (*entries)[i].start_address,
               (*entries)[i].kbytes,
               (*entries)[i].rss,
               (*entries)[i].dirty,
               (*entries)[i].r,
               (*entries)[i].w,
               (*entries)[i].x,
               (*entries)[i].mapping);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <pid>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int pid = atoi(argv[1]);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pmap -x %d", pid);

    FILE *fp = popen(cmd, "r");
    if (fp == NULL) {
        perror("popen");
        return EXIT_FAILURE;
    }

    struct pmap_entry *entries = malloc(sizeof(struct pmap_entry) * INITIAL_CAPACITY);
    if (!entries) {
        perror("malloc");
        pclose(fp);
        return EXIT_FAILURE;
    }

    int count = 0;
    if (parse_pmap_output(&fp, &entries, &count) != 0)
        return EXIT_FAILURE;

    pclose(fp);

    print_pmap_entries(&entries, count);

    free(entries);

    return EXIT_SUCCESS;
}

