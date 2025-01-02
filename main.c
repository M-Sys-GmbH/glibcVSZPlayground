#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <pthread.h>
#include <getopt.h>
#include <limits.h>
#include <malloc.h>

# if __WORDSIZE == 32
#  define GLIBC_ARENA_SIZE_IN_KBYTES 512
# else
#  define GLIBC_ARENA_SIZE_IN_KBYTES (2 * 4 * 1024 * sizeof(long))
# endif

#define DEFAULT_NUM_THREADS 20
#define DEFAULT_MALLOC_COUNT 1
#define PMAP_INITIAL_CAPACITY 50
#define PMAP_MAX_MAPPING_PATH 1024
#define PMAP_MAX_ADDRESS_LEN  64

volatile sig_atomic_t running = 1;
int malloc_enabled = 0;
int malloc_fill_enabled = 0;
size_t malloc_size = 0;
int stack_size_given = 0;
int malloc_count = DEFAULT_MALLOC_COUNT;
int num_threads = DEFAULT_NUM_THREADS;
size_t stack_size = 0;

struct pmap_entry {
    unsigned long long start_address;
    int kbytes;
    int rss;
    int dirty;
    char r;
    char w;
    char x;
    char mapping[PMAP_MAX_MAPPING_PATH];
};

struct malloc_info_entry {
    unsigned long long malloc_start_address;
    size_t malloc_size;
    unsigned long long malloc_end_address;
};

struct thread_info_entry {
    long thread_id;
    pid_t tid;
    unsigned long long stack_start_address;
    size_t stack_size;
    unsigned long long stack_end_address;
    struct malloc_info_entry *malloc_info_entries;
    size_t malloc_entry_count;
    int finished;
};

static int ranges_overlap(unsigned long long start1, unsigned long long end1,
                          unsigned long long start2, unsigned long long end2) {
    return (start1 < end2) && (end1 > start2);
}

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
    int capacity = PMAP_INITIAL_CAPACITY;
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

        char address[PMAP_MAX_ADDRESS_LEN];
        int kbytes, rss, dirty;
        char mode[8];
        char mapping[PMAP_MAX_MAPPING_PATH];
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
            strncpy(entries[*count].mapping, m, PMAP_MAX_MAPPING_PATH);
            entries[*count].mapping[PMAP_MAX_MAPPING_PATH - 1] = '\0';
        } else {
            entries[*count].mapping[0] = '\0';
        }

        (*count)++;
    }
    return entries;
}

int check_pmap_entry_type(struct pmap_entry *p_current, struct pmap_entry *p_before, struct pmap_entry *p_after, struct thread_info_entry *thread_entries, int thread_entry_count) {
    if (p_after) {
        if (p_current->start_address + 0x200000000000 < p_after->start_address) {
            printf("---------------- Main Thread - HEAP ---------------\n");
            return 1;
        }

        // Thread Arena Check - Part 1
        if (GLIBC_ARENA_SIZE_IN_KBYTES == p_current->kbytes + p_after->kbytes && p_current->r == 'r') {
            printf("---------------- Thread Arena - HEAP --------------\n");
        }
    }

    if (p_before) {
        // Thread Arena Check - Part 2
        if (GLIBC_ARENA_SIZE_IN_KBYTES == p_current->kbytes + p_before->kbytes && p_current->r == '-') {
            return 1;
        }
    }

    // Thread Stack Check
    for (int i = 0; i < thread_entry_count; i++) {
        struct thread_info_entry t_entry = thread_entries[i];
        unsigned long long pmap_start = p_current->start_address;
        unsigned long long pmap_end = p_current->start_address + (p_current->kbytes * 1024ULL);

        if (ranges_overlap(pmap_start, pmap_end, 
                           t_entry.stack_start_address, t_entry.stack_end_address)) {
            printf("---------------- Thread %ld - STACK ---------------\n", t_entry.thread_id);
            return 1;
        }
    }
    return 0;
}

void print_pmap_entry(struct pmap_entry p_entry) {
    printf("%llx %8d %8d %8d %c%c%c %s\n",
           p_entry.start_address,
           p_entry.kbytes,
           p_entry.rss,
           p_entry.dirty,
           p_entry.r,
           p_entry.w,
           p_entry.x,
           p_entry.mapping);
}

void print_malloc_entries(struct thread_info_entry thread_entry, unsigned long long pmap_start, unsigned long long pmap_end) {
    for (size_t j = 0; j < thread_entry.malloc_entry_count; j++) {
        if (ranges_overlap(pmap_start, pmap_end,
                           thread_entry.malloc_info_entries[j].malloc_start_address,
                           thread_entry.malloc_info_entries[j].malloc_end_address)) {
            printf("    Thread %ld (TID: %d)", thread_entry.thread_id, thread_entry.tid);
            size_t alloc_size = thread_entry.malloc_info_entries[j].malloc_size;
            printf(" - Malloc #%ld: [0x%llx - 0x%llx] (size: %zu bytes)\n",
                   j+1,
                   thread_entry.malloc_info_entries[j].malloc_start_address,
                   thread_entry.malloc_info_entries[j].malloc_end_address,
                   alloc_size);
        }
    }
}

void create_output(struct pmap_entry *pmap_entries, int pmap_entry_count, struct thread_info_entry *thread_entries, int thread_entry_count) {
    printf("%-12s %8s %8s %8s %s%s%s %s\n",
           "Address", "Kbytes", "RSS", "Dirty", "R", "W", "X", "Mapping");
    printf("---------------------------------------------------\n");
    for (int m = 0; m < pmap_entry_count; m++) {
        struct pmap_entry *p_current = &pmap_entries[m];
        struct pmap_entry *p_before = NULL;
        struct pmap_entry *p_after = NULL;
        int endline_needed = 0;

        if (m + 1 < pmap_entry_count)
            p_after = &pmap_entries[m+1];
        if (m - 1 > 0)
            p_before = &pmap_entries[m-1];

        endline_needed = check_pmap_entry_type(p_current, p_before, p_after, thread_entries, thread_entry_count);

        print_pmap_entry(*p_current);

        for (int i = 0; i < thread_entry_count; i++) {
            struct thread_info_entry t_entry = thread_entries[i];
            unsigned long long pmap_start = p_current->start_address;
            unsigned long long pmap_end = p_current->start_address + (p_current->kbytes * 1024ULL);

            if (ranges_overlap(pmap_start, pmap_end, 
                               t_entry.stack_start_address, t_entry.stack_end_address)) {
                printf("    Thread %ld (TID: %d)", t_entry.thread_id, t_entry.tid);
                printf(" - Stack: [0x%llx - 0x%llx] (size: %zu bytes)\n", 
                       t_entry.stack_start_address, 
                       t_entry.stack_end_address, 
                       t_entry.stack_size);
            }
            print_malloc_entries(t_entry, pmap_start, pmap_end);
        }

        if (endline_needed) {
            printf("---------------------------------------------------\n");
        }
    }
}

int get_stack_info(void **stack_addr, size_t *stack_size, void **stack_end_addr) {
    pthread_t self = pthread_self();
    pthread_attr_t attr;

    int ret = pthread_getattr_np(self, &attr);
    if (ret != 0) {
        perror("pthread_getattr_np");
        return -1;
    }

    pthread_attr_getstack(&attr, &*stack_addr, &*stack_size);
    pthread_attr_destroy(&attr);

    *stack_end_addr = (void *)((char *)(*stack_addr) + *(stack_size));

    return 0;
}

void** malloc_allocate_function(long thread_id, struct malloc_info_entry *entries) {
    void **allocated_memory = malloc(malloc_count * sizeof(void*));
    if (allocated_memory == NULL) {
        fprintf(stderr, "Thread %ld failed to allocate memory for malloc pointers\n", thread_id);
        return NULL;
    }

    for (int i = 0; i < malloc_count; i++) {
        allocated_memory[i] = malloc(malloc_size);
        if (allocated_memory[i] != NULL) {
            if (malloc_fill_enabled) {
                memset(allocated_memory[i], 0xAA, malloc_size);
            }
            void *malloc_end_addr = (char *)allocated_memory[i] + malloc_size;
            entries[i].malloc_start_address = (unsigned long long) allocated_memory[i];
            entries[i].malloc_size = malloc_size;
            entries[i].malloc_end_address = (unsigned long long) malloc_end_addr;
        } else {
            entries[i].malloc_start_address = 0;
            entries[i].malloc_size = 0;
            entries[i].malloc_end_address = 0;
        }
    }
    return allocated_memory;
}

void malloc_deallocate_function(void **allocated_memory) {
    if (!allocated_memory) return;
    for (int i = 0; i < malloc_count; i++) {
         if (allocated_memory[i]) {
             free(allocated_memory[i]);
         }
     }
     free(allocated_memory);
}

void* thread_function(void *arg) {
    struct thread_info_entry *tinfo = (struct thread_info_entry *)arg;
    long thread_id = tinfo->thread_id;
    void *stack_addr = NULL;
    size_t stack_size;
    void *stack_end_addr = NULL;
    void **allocated_memory = NULL;

    if (get_stack_info(&stack_addr, &stack_size, &stack_end_addr) != 0) {
        return NULL;
    }

    // Thread Info Entry Init
    tinfo->thread_id = thread_id;
    tinfo->tid = syscall(SYS_gettid);
    tinfo->stack_start_address = (unsigned long long) stack_addr;
    tinfo->stack_size = stack_size;
    tinfo->stack_end_address = (unsigned long long) stack_end_addr;
    if (tinfo->malloc_entry_count > 0 && tinfo->malloc_info_entries == NULL) {
        fprintf(stderr, "Thread %ld failed to allocate memory for thread_info_entry pointerrs\n", thread_id);
        return NULL;
    }

    if (malloc_enabled) {
        allocated_memory = malloc_allocate_function(thread_id, tinfo->malloc_info_entries);
        if (!allocated_memory) {
            return NULL;
        }
    }

    tinfo->finished = 1;

    while (running) {
        usleep(100000);
    }

    if (allocated_memory != NULL) {
        malloc_deallocate_function(allocated_memory);
        allocated_memory = NULL;
    }

    return NULL;
}

int set_malloc_arena_number(int count) {
    if (mallopt(M_ARENA_MAX, count) == 0) {
        fprintf(stderr, "ERROR while trying to set MALLOC_ARENA_MAX\n");
        return 1;
    }
    return 0;
}

int set_malloc_mmap_threshold_in_bytes(size_t size) {
    if (mallopt(M_MMAP_THRESHOLD, size) == 0) {
        fprintf(stderr, "ERROR while trying to set M_MMAP_THRESHOLD\n");
        return 1;
    }
    return 0;
}

void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -n, --num-threads <num>                   Number of threads (default: %d)\n", DEFAULT_NUM_THREADS);
    printf("  -s, --thread-stack-size <size>            Stack size for each thread in bytes (default: system default)\n");
    printf("  -m, --malloc-sparse-inside-thread <size>  Allocate memory inside each thread without initialization\n");
    printf("  -f, --malloc-filled-inside-thread <size>  Allocate memory inside each thread and fill with 0xAA\n");
    printf("  -c, --count-of-mallocs <number>           Number of malloc calls inside each thread (default: %d)\n", DEFAULT_MALLOC_COUNT);
    printf("  -a, --malloc-arena-max <size>             Set number of arenas via MALLOC_ARENA_MAX (default: depends on arch and cpu core count)\n");
    printf("  -t, --mmap-threshold <size>               Set threshold number of bytes by which memory allocations are done via mmap instead of using heap/arenas (default: 131072)\n");
    printf("  -h, --help                                Show this help message\n");
}

void parse_arguments(int *argc, char *argv[]) {
    int opt;
    int option_index = 0;
    extern int optind;

    // Define the options
    static struct option long_options[] = {
        {"num-threads", required_argument, 0, 'n'},
        {"thread-stack-size", required_argument, 0, 's'},
        {"malloc-sparse-inside-thread", required_argument, 0, 'm'},
        {"malloc-filled-inside-thread", required_argument, 0, 'f'},
        {"count-of-mallocs", required_argument, 0, 'c'},
        {"malloc-arena-max", required_argument, 0, 'a'},
        {"mmap-threshold", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(*argc, argv, "n:s:m:f:c:a:t:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'n':
                if (sscanf(optarg, "%d", &num_threads) != 1 || num_threads <= 0) {
                    fprintf(stderr, "Invalid number of threads: %s. Must be a positive integer.\n", optarg);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 's':
                stack_size_given = 1;
                if (sscanf(optarg, "%zu", &stack_size) != 1 || stack_size < (size_t)PTHREAD_STACK_MIN) {
                    fprintf(stderr, "Invalid stack size: %s. Must be at least %ld bytes.\n", optarg, PTHREAD_STACK_MIN);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'm':
                malloc_enabled = 1;
                malloc_fill_enabled = 0;
                if (sscanf(optarg, "%zu", &malloc_size) != 1 || malloc_size <= 0) {
                    fprintf(stderr, "Invalid malloc size: %s. Must be a positive integer.\n", optarg);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'f':
                malloc_enabled = 1;
                malloc_fill_enabled = 1;
                if (sscanf(optarg, "%zu", &malloc_size) != 1 || malloc_size <= 0) {
                    fprintf(stderr, "Invalid malloc size: %s. Must be a positive integer.\n", optarg);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'c':
                if (sscanf(optarg, "%d", &malloc_count) != 1 || malloc_count <= 0) {
                    fprintf(stderr, "Invalid count of mallocs: %s. Must be a positive integer.\n", optarg);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'a':
                int arenas = 0;
                if (sscanf(optarg, "%d", &arenas) != 1 || arenas <= 0) {
                    fprintf(stderr, "Invalid number of arenas: %s. Must be a positive integer.\n", optarg);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                if (set_malloc_arena_number(arenas) != 0)
                    exit(EXIT_FAILURE);
                break;
            case 't':
                int mmap_threshold = 0;
                if (sscanf(optarg, "%d", &mmap_threshold) != 1 || mmap_threshold <= 0) {
                    fprintf(stderr, "Invalid size of mmap threshold: %s. Must be a positive integer.\n", optarg);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                if (set_malloc_mmap_threshold_in_bytes(mmap_threshold) != 0)
                    exit(EXIT_FAILURE);
                break;
            case 'h':
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Check if there are any non-option arguments left
    if (optind < *argc) {
        fprintf(stderr, "Unknown argument(s): ");
        while (optind < *argc) {
            fprintf(stderr, "%s ", argv[optind++]);
        }
        fprintf(stderr, "\n");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
}

void set_stack_size(pthread_attr_t *attr) {
    if (stack_size_given) {
        if (pthread_attr_setstacksize(attr, stack_size) != 0) {
            fprintf(stderr, "Error setting stack size (%zu bytes). Using system default stack size.\n", stack_size);
        }
    }
}

int create_threads(pthread_t *threads, struct thread_info_entry *entry) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    set_stack_size(&attr);

    for (long i = 0; i < num_threads; i++) {
        entry[i].thread_id = i;
        entry[i].finished = 0;
        entry[i].malloc_entry_count = malloc_count;
        entry[i].malloc_info_entries = malloc(sizeof(struct malloc_info_entry) * entry[i].malloc_entry_count);

        if (pthread_create(&threads[i], &attr, thread_function, (void*)&entry[i]) != 0) {
            perror("pthread_create");
            pthread_attr_destroy(&attr);
            return -1;
        }
    }

    pthread_attr_destroy(&attr);

    return 0;
}

void wait_for_threads_to_finish(struct thread_info_entry *entries) {
    for (int i = 0; i < num_threads; i++) {
        while (!entries[i].finished) {
            usleep(1000);
        }
    }
}

struct pmap_entry *get_pmap_analysis(int pid, int *count) {
    FILE *fp = execute_pmap_cmd(pid);
    if (!fp) {
        return NULL;
    }

    struct pmap_entry *entries = parse_pmap_output(fp, count);

    pclose(fp);

    if (!entries) {
        return NULL;
    }
    return entries;
}

void stop_threads(pthread_t *threads) {
    // Set stop signal for threads
    running = 0;

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
}

void free_thread_info_entries(struct thread_info_entry *entries) {
    for (int i = 0; i < num_threads; i++) {
          if (entries[i].malloc_info_entries != NULL) {
              free(entries[i].malloc_info_entries);
              entries[i].malloc_info_entries = NULL;
          }
    }
    free(entries);
    entries = NULL;
}

int main(int argc, char *argv[]) {

    parse_arguments(&argc, argv);

    struct thread_info_entry *thread_info_entries = malloc(sizeof(struct thread_info_entry) * num_threads);
    if (!thread_info_entries) {
        perror("malloc thread_info entries failed!");
        return EXIT_FAILURE;
    }

    pthread_t threads[num_threads];

    if (create_threads(threads, thread_info_entries) != 0) {
        return EXIT_FAILURE;
    }

    wait_for_threads_to_finish(thread_info_entries);

    int pmap_entry_count = 0;
    struct pmap_entry *pmap_entries = get_pmap_analysis(getpid(), &pmap_entry_count);
    if (!pmap_entries) {
        free_thread_info_entries(thread_info_entries);
        return EXIT_FAILURE;
    }

    create_output(pmap_entries, pmap_entry_count, thread_info_entries, num_threads);

    stop_threads(threads);

    free_thread_info_entries(thread_info_entries);
    free(pmap_entries);

    return 0;
}
