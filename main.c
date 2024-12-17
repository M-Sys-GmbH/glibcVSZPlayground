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

// Signal handler to handle Ctrl+C (SIGINT)
void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

int malloc_function(void*** allocated_memory, long thread_id, char *output_buffer, int *offset) {
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
        } else {
            *offset += snprintf(output_buffer + *offset, sizeof(output_buffer) - *offset,
                               "Thread %ld failed to allocate %zu bytes on malloc %d\n",
                               thread_id, malloc_size, i);
        }
    }
    return 0;
}

void* thread_function(void* arg) {
    long thread_id = (long)arg;
    pthread_t self = pthread_self();
    pid_t tid = syscall(SYS_gettid);

    // Get stack attributes
    pthread_attr_t attr;
    void *stack_addr;
    size_t stack_size;

    int ret = pthread_getattr_np(self, &attr);
    if (ret != 0) {
        perror("pthread_getattr_np");
        return NULL;
    }

    pthread_attr_getstack(&attr, &stack_addr, &stack_size);
    pthread_attr_destroy(&attr);

    // Calculate the last address of the stack
    void *stack_end_addr = (void *)((char *)stack_addr + stack_size);

    void **allocated_memory = NULL;
    char output_buffer[4096];
    int offset = 0;

    offset += snprintf(output_buffer + offset, sizeof(output_buffer) - offset,
                       "Thread %ld started. TID: %d, Stack Address: %p, Stack End Address: %p, Stack Size: %zu bytes\n",
                       thread_id, tid, stack_addr, stack_end_addr, stack_size);

    if (malloc_enabled) {
        if (malloc_function(&allocated_memory, thread_id, output_buffer, &offset)) {
            fprintf(stderr, "Thread %ld failed to allocate memory for malloc pointers\n", thread_id);
            return NULL;
        }
    }

    // Print all thread information in one go
    printf("%s", output_buffer);

    while (running) {
        usleep(100000);
    }

    if (allocated_memory != NULL) {
        for (int i = 0; i < malloc_count; i++) {
            if (allocated_memory[i] != NULL) {
                free(allocated_memory[i]);
            }
        }
        free(allocated_memory);
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

int main(int argc, char *argv[]) {
    int num_threads = DEFAULT_NUM_THREADS;
    size_t stack_size = 0;

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

    int opt;
    int option_index = 0;
    extern int optind;

    while ((opt = getopt_long(argc, argv, "n:s:m:f:c:h", long_options, &option_index)) != -1) {
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
            case 'h':
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Check if there are any non-option arguments left
    if (optind < argc) {
        fprintf(stderr, "Unknown argument(s): ");
        while (optind < argc) {
            fprintf(stderr, "%s ", argv[optind++]);
        }
        fprintf(stderr, "\n");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    pthread_t threads[num_threads];
    pthread_attr_t attr;

    // Initialize thread attribute
    pthread_attr_init(&attr);
    if (stack_size_given) {
        if (pthread_attr_setstacksize(&attr, stack_size) != 0) {
            fprintf(stderr, "Error setting stack size (%zu bytes). Using system default stack size.\n", stack_size);
        }
    }

    // Set up signal handler for SIGINT
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // Create threads
    for (long i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], &attr, thread_function, (void*)i) != 0) {
            perror("pthread_create");
            pthread_attr_destroy(&attr);
            exit(EXIT_FAILURE);
        }
    }

    pthread_attr_destroy(&attr);

    sleep(3);
    printf("#################################\n");
    printf("Program PID: %d\n", getpid());
    printf("Program running. Press Ctrl+C to exit.\n");
    printf("#################################\n");

    // Wait until Ctrl+C is pressed
    while (running) {
        sleep(1);
    }

    // Wait for all threads to finish
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("All threads have exited. Program terminating.\n");
    return 0;
}
