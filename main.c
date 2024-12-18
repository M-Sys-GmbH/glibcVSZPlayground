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

#define DEFAULT_NUM_THREADS 20
#define DEFAULT_MALLOC_COUNT 1

volatile sig_atomic_t running = 1;
int malloc_enabled = 0;
int malloc_fill_enabled = 0;
size_t malloc_size = 0;
int stack_size_given = 0;
int malloc_count = DEFAULT_MALLOC_COUNT;
int num_threads = DEFAULT_NUM_THREADS;
size_t stack_size = 0;

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
};

void handle_sigint(int sig) {
    (void)sig;
    running = 0;
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

int malloc_allocate_function(void*** allocated_memory, long thread_id, struct malloc_info_entry **entries, char *output_buffer, int *offset) {
    *allocated_memory = malloc(malloc_count * sizeof(void*));
    if (*allocated_memory == NULL) {
        fprintf(stderr, "Thread %ld failed to allocate memory for malloc pointers\n", thread_id);
        return -1;
    }

    for (int i = 0; i < malloc_count; i++) {
        (*allocated_memory)[i] = malloc(malloc_size);
        if ((*allocated_memory)[i] != NULL) {
            if (malloc_fill_enabled) {
                memset((*allocated_memory)[i], 0xAA, malloc_size);
            }
            void *malloc_end_addr = (void *)((char *)(*allocated_memory)[i] + malloc_size);
            *offset += snprintf(output_buffer + *offset, sizeof(output_buffer) - *offset,
                               "\tAllocated %zu bytes at address %p to %p\n",
                               malloc_size, (*allocated_memory)[i], malloc_end_addr);
            (*entries)[i].malloc_start_address = (unsigned long long) (*allocated_memory)[i];
            (*entries)[i].malloc_size = malloc_size;
            (*entries)[i].malloc_end_address = (unsigned long long) (malloc_end_addr);
        } else {
            *offset += snprintf(output_buffer + *offset, sizeof(output_buffer) - *offset,
                               "Thread %ld failed to allocate %zu bytes on malloc %d\n",
                               thread_id, malloc_size, i);
            (*entries)[i].malloc_start_address = 0;
            (*entries)[i].malloc_size = 0;
            (*entries)[i].malloc_end_address = 0;
        }
    }
    return 0;
}

void malloc_deallocate_function(void ***allocated_memory) {
    for (int i = 0; i < malloc_count; i++) {
         if ((*allocated_memory)[i] != NULL) {
             free((*allocated_memory)[i]);
         }
     }
     free(*allocated_memory);
}

void* thread_function(void *arg) {
    struct thread_info_entry *tinfo = (struct thread_info_entry *)arg;

    char output_buffer[40960];
    int offset = 0;

    long thread_id = tinfo->thread_id;
    pid_t tid = syscall(SYS_gettid);

    void *stack_addr = NULL;
    size_t stack_size;
    void *stack_end_addr = NULL;

    void **allocated_memory = NULL;

    if (get_stack_info(&stack_addr, &stack_size, &stack_end_addr) != 0) {
        return NULL;
    }

    offset += snprintf(output_buffer + offset, sizeof(output_buffer) - offset,
                       "Thread %ld started. TID: %d, Stack Address: %p, Stack End Address: %p, Stack Size: %zu bytes\n",
                       thread_id, tid, stack_addr, stack_end_addr, stack_size);

    // Thread Info Entry Init
    tinfo->thread_id = thread_id;
    tinfo->tid = syscall(SYS_gettid);
    tinfo->stack_start_address = (unsigned long long) stack_addr;
    tinfo->stack_size = stack_size;
    tinfo->stack_end_address = (unsigned long long) stack_end_addr;
    tinfo->malloc_entry_count = malloc_count;
    tinfo->malloc_info_entries = malloc(sizeof(struct malloc_info_entry) * tinfo->malloc_entry_count);
    if (tinfo->malloc_entry_count > 0 && tinfo->malloc_info_entries == NULL) {
        fprintf(stderr, "Thread %ld failed to allocate memory for thread_info_entry pointerrs\n", thread_id);
        return NULL;
    }

    if (malloc_enabled) {
        if (malloc_allocate_function(&allocated_memory, thread_id, &tinfo->malloc_info_entries, output_buffer, &offset) != 0) {
            return NULL;
        }
    }

    // Print all thread information in one go
    printf("%s", output_buffer);

    while (running) {
        usleep(100000);
    }

    if (allocated_memory != NULL) {
        malloc_deallocate_function(&allocated_memory);
    }

    return NULL;
}

void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -n, --num-threads <num>                   Number of threads (default: %d)\n", DEFAULT_NUM_THREADS);
    printf("  -s, --thread-stack-size <size>            Stack size for each thread in bytes (default: system default)\n");
    printf("  -m, --malloc-sparse-inside-thread <size>  Allocate memory inside each thread without initialization\n");
    printf("  -f, --malloc-filled-inside-thread <size>  Allocate memory inside each thread and fill with 0xAA\n");
    printf("  -c, --count-of-mallocs <number>           Number of malloc calls inside each thread (default: %d)\n", DEFAULT_MALLOC_COUNT);
    printf("  -h, --help                                Show this help message\n");
}

void setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

void parse_arguments(int *argc, char **argv[]) {
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
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(*argc, *argv, "n:s:m:f:c:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'n':
                if (sscanf(optarg, "%d", &num_threads) != 1 || num_threads <= 0) {
                    fprintf(stderr, "Invalid number of threads: %s. Must be a positive integer.\n", optarg);
                    print_usage(*argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 's':
                stack_size_given = 1;
                if (sscanf(optarg, "%zu", &stack_size) != 1 || stack_size < (size_t)PTHREAD_STACK_MIN) {
                    fprintf(stderr, "Invalid stack size: %s. Must be at least %ld bytes.\n", optarg, PTHREAD_STACK_MIN);
                    print_usage(*argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'm':
                malloc_enabled = 1;
                malloc_fill_enabled = 0;
                if (sscanf(optarg, "%zu", &malloc_size) != 1 || malloc_size <= 0) {
                    fprintf(stderr, "Invalid malloc size: %s. Must be a positive integer.\n", optarg);
                    print_usage(*argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'f':
                malloc_enabled = 1;
                malloc_fill_enabled = 1;
                if (sscanf(optarg, "%zu", &malloc_size) != 1 || malloc_size <= 0) {
                    fprintf(stderr, "Invalid malloc size: %s. Must be a positive integer.\n", optarg);
                    print_usage(*argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'c':
                if (sscanf(optarg, "%d", &malloc_count) != 1 || malloc_count <= 0) {
                    fprintf(stderr, "Invalid count of mallocs: %s. Must be a positive integer.\n", optarg);
                    print_usage(*argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'h':
                print_usage(*argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(*argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Check if there are any non-option arguments left
    if (optind < *argc) {
        fprintf(stderr, "Unknown argument(s): ");
        while (optind < *argc) {
            fprintf(stderr, "%s ", *argv[optind++]);
        }
        fprintf(stderr, "\n");
        print_usage(*argv[0]);
        exit(EXIT_FAILURE);
    }
}

void set_stack_size(pthread_attr_t *attr) {
    if (stack_size_given) {
        if (pthread_attr_setstacksize(&*attr, stack_size) != 0) {
            fprintf(stderr, "Error setting stack size (%zu bytes). Using system default stack size.\n", stack_size);
        }
    }
}

int create_threads(pthread_t *threads, struct thread_info_entry **info) {
    struct thread_info_entry *entry = *info;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    set_stack_size(&attr);

    for (long i = 0; i < num_threads; i++) {
        entry[i].thread_id = i;
        if (pthread_create(&threads[i], &attr, thread_function, (void*)&entry[i]) != 0) {
            perror("pthread_create");
            pthread_attr_destroy(&attr);
            return -1;
        }
    }

    pthread_attr_destroy(&attr);

    return 0;
}

int main(int argc, char *argv[]) {

    setup_signal_handler();

    parse_arguments(&argc, &argv);

    struct thread_info_entry *entries = malloc(sizeof(struct thread_info_entry) * num_threads);
    if (!entries) {
        perror("malloc thread_info entries failed!");
        return EXIT_FAILURE;
    }

    pthread_t threads[num_threads];

    if (create_threads(threads, &entries) != 0) {
        exit(EXIT_FAILURE);
    }

    sleep(3);
    printf("#################################\n");
    printf("Program PID: %d\n", getpid());
    printf("Program running. Press Ctrl+C to exit.\n");
    printf("#################################\n");

    while (running) {
        sleep(1);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // Free the entries
    for (int i = 0; i < num_threads; i++) {
          if (entries[i].malloc_info_entries != NULL) {
              free(entries[i].malloc_info_entries);
              entries[i].malloc_info_entries = NULL;
          }
    }
    free(entries);
    entries = NULL;

    printf("All threads have exited. Program terminating.\n");
    return 0;
}
