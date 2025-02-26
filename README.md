# Testing Program for glibc's pthreads and malloc()

In a project we had the task to investigate the high VSZ (Virtual Memory Size) usage on an embedded Linux system.
The software the client put on the embedded Linux device was highly multithreaded, it turned out that there were 2 reasons for the high VSZ:
- pthread stack size for each thread
- glibc (thread) arenas created by malloc() calls within the thread functions

With the cli tool **pmap**, one can display the memory map of a process with all sections and the corresponding addresses and some additional information. We found, that to get an better understanding of the way of working the output needs some extra information.
So we created this testing/playing tool, which creates a number of threads an allocates/deallocates memory inside the threads and prints a pmap like result, but will tell which heap & stack of which thread is where.

## pthread Stack Size
Each thread requires an own thread stack, the size of this thread stack can be altered, but it defaults on modern 64-bit systems to 8 MB.  
One can alter the default value with:
- [pthread_attr_setstacksize()](https://www.man7.org/linux/man-pages/man3/pthread_attr_setstacksize.3.html)
- [Setting a new ulimit](https://www.baeldung.com/linux/bash-stack-size-limit) 

### Important Considerations:
- **Minimum Size**: There's a platform-dependent minimum stack size (PTHREAD_STACK_MIN) below which you cannot set the stack size
- **Memory Impact**: Each thread's stack contributes to the process's VSZ, even if only a small portion is actually used (RSS)
- **Guard Pages**: Thread stacks typically include guard pages to detect stack overflows
- **Alignment**: Stack sizes are often rounded up to page boundaries

### Stack Size Trade-offs:
- **Too Large**: Wastes virtual address space, as shown in our examples (high VSZ)
- **Too Small**: Risks stack overflow if thread functions use deep recursion or large local variables

### Determining Appropriate Stack Size:
For embedded systems or applications with many threads, consider reducing the default stack size after analyzing actual stack usage patterns of your threads.

[Source - pthread_create](https://www.man7.org/linux/man-pages/man3/pthread_create.3.html)

> Under the NPTL threading implementation, if the RLIMIT_STACK soft
resource limit at the time the program started has any value
other than "unlimited", then it determines the default stack size
of new threads.  Using pthread_attr_setstacksize(3), the stack
size attribute can be explicitly set in the attr argument used to
create a thread, in order to obtain a stack size other than the
default.  If the RLIMIT_STACK resource limit is set to
"unlimited", a per-architecture value is used for the stack size:
2 MB on most architectures; 4 MB on POWER and Sparc-64.  

> The program below demonstrates the use of pthread_create(), as
  well as a number of other functions in the pthreads API.  
  In the following run, on a system providing the NPTL threading
  implementation, the stack size defaults to the value given by the
  "stack size" resource limit:   
  \$ ulimit -s  
  8192            # The stack size limit is 8 MB (0x800000 bytes)  
  \$ ./a.out hola salut servus  
  Thread 1: top of stack near 0xb7dd03b8; argv_string=hola  
  Thread 2: top of stack near 0xb75cf3b8; argv_string=salut  
  Thread 3: top of stack near 0xb6dce3b8; argv_string=servus  
  Joined with thread 1; returned value was HOLA  
  Joined with thread 2; returned value was SALUT  
  Joined with thread 3; returned value was SERVUS

## malloc() ThreadArenas
### What are Thread Arenas?
Thread arenas are memory regions managed by glibc's malloc implementation to handle memory allocations in multithreaded programs. Each arena contains a heap from which memory is allocated when a thread calls `malloc()`.

### Key Characteristics of Thread Arenas
- **Per-Thread Memory Management**: By default, glibc reduces lock contention by creating multiple memory arenas and assigning threads to these arenas in a round-robin fashion, rather than creating a separate arena for each thread.
- **Arena Size**: Each arena typically reserves a large chunk of virtual memory (64MB+) but only commits physical memory as needed
- **Arena Limit**: The maximum number of arenas is controlled by `MALLOC_ARENA_MAX` environment variable
- **Default Arena Count**: If not explicitly set, the default maximum is calculated as:
  - 8 × (number of CPU cores) for 64-bit systems
  - 2 × (number of CPU cores) for 32-bit systems

### Impact on Memory Usage
- **Virtual Memory Overhead**: Each arena reserves a significant amount of virtual address space (VSZ), even if only a small portion is actually used
- **Physical Memory Usage**: Only the portions of arenas that are actually used consume physical memory (RSS)
- **Memory Fragmentation**: Multiple arenas can lead to increased memory fragmentation across the process

### Controlling Thread Arena Behavior

#### Environment Variables

- **MALLOC_ARENA_MAX**: Limits the total number of arenas
  ```bash
  export MALLOC_ARENA_MAX=2  # Limit to 2 arenas total
  ```

- **MALLOC_TRIM_THRESHOLD**: Controls when unused memory is returned to the system
  ```bash
  export MALLOC_TRIM_THRESHOLD=131072  # Return memory when 128KB is free
  ```

- **MALLOC_MMAP_THRESHOLD**: Sets the size threshold above which allocations use mmap() instead of arenas
  ```bash
  export MALLOC_MMAP_THRESHOLD=131072  # Use mmap for allocations ≥ 128KB
  ```

#### Programmatic Control

- **mallopt()**: Can be used to set various malloc parameters at runtime
  ```c
  #include <malloc.h>
  mallopt(M_ARENA_MAX, 2);  // Limit to 2 arenas
  mallopt(M_MMAP_THRESHOLD, 131072);  // Use mmap for allocations ≥ 128KB
  ```

### Optimization Strategies

- **Reduce Arena Count**: For memory-constrained systems, limit the number of arenas
- **Thread Pooling**: Reuse threads instead of creating/destroying them to limit arena creation
- **Large Allocation Threshold**: Adjust the mmap threshold to bypass arenas for larger allocations
- **Memory Release**: Consider calling `malloc_trim(0)` periodically to release unused memory

### Example Impact
As demonstrated by our test program, a process with just 3 threads and minimal actual memory usage (1.2MB RSS) can show a virtual memory footprint of over 200MB, with most of this coming from thread stacks (8MB × 3) and thread arenas (64MB × 3).

### Further Reading
- [glibc malloc internals](https://sourceware.org/glibc/wiki/MallocInternals)
- [Understanding glibc malloc](https://sploitfun.wordpress.com/2015/02/10/understanding-glibc-malloc/)
- [Customizing malloc behavior](https://www.gnu.org/software/libc/manual/html_node/Malloc-Tunable-Parameters.html)

## Usage
```
Usage: ./glibcVSZPlayground [options]
Options:
  -n, --num-threads <num>                   Number of threads (default: 20)
  -s, --thread-stack-size <size>            Stack size for each thread in bytes (default: system default)
  -m, --malloc-sparse-inside-thread <size>  Allocate memory inside each thread without initialization
  -f, --malloc-filled-inside-thread <size>  Allocate memory inside each thread and fill with 0xAA
  -c, --count-of-mallocs <number>           Number of malloc calls inside each thread (default: 1)
  -a, --malloc-arena-max <size>             Set number of arenas via MALLOC_ARENA_MAX (default: depends on arch and cpu core count)
  -t, --mmap-threshold <size>               Set threshold number of bytes by which memory allocations are done via mmap instead of using heap/arenas (default: 131072)
  -h, --help                                Show this help message
```

## Example
### Test Tool Output
We run the test tool with following parameters:
- -n 3    | 3 Threads
- -m 1024 | malloc() call with 1024 bytes, sparse (won't be used/set) in thread function
- -c      | malloc() call will happen 5 times

What you can see below is the following:
- 3 Threads are started
- Thread stacks with stack addresses (start, end) and stack size for each thread
- Thread arenas with malloc() call sizes and address range of allocations for each thread
- PID of program

```
./glibcVSZPlayground -n 3 -m 1024 -c 5
Address        Kbytes      RSS    Dirty RWX Mapping
---------------------------------------------------
aaaae8580000       20       20        0 r-x glibcVSZPlayground
aaaae8594000        4        4        4 r-- glibcVSZPlayground
aaaae8595000        4        4        4 rw- glibcVSZPlayground
---------------- Main Thread - HEAP ---------------
aaaaf0f81000      132       20       20 rw- [ anon ]
---------------------------------------------------
---------------- Thread Arena - HEAP --------------
ffff9c000000      132       16       16 rw- [ anon ]
    Thread 2 (TID: 53187) - Malloc #1: [0xffff9c000c70 - 0xffff9c001070] (size: 1024 bytes)
    Thread 2 (TID: 53187) - Malloc #2: [0xffff9c001080 - 0xffff9c001480] (size: 1024 bytes)
    Thread 2 (TID: 53187) - Malloc #3: [0xffff9c001490 - 0xffff9c001890] (size: 1024 bytes)
    Thread 2 (TID: 53187) - Malloc #4: [0xffff9c0018a0 - 0xffff9c001ca0] (size: 1024 bytes)
    Thread 2 (TID: 53187) - Malloc #5: [0xffff9c001cb0 - 0xffff9c0020b0] (size: 1024 bytes)
ffff9c021000    65404        0        0 --- [ anon ]
---------------------------------------------------
---------------- Thread Arena - HEAP --------------
ffffa4000000      132       16       16 rw- [ anon ]
    Thread 1 (TID: 53186) - Malloc #1: [0xffffa4000c70 - 0xffffa4001070] (size: 1024 bytes)
    Thread 1 (TID: 53186) - Malloc #2: [0xffffa4001080 - 0xffffa4001480] (size: 1024 bytes)
    Thread 1 (TID: 53186) - Malloc #3: [0xffffa4001490 - 0xffffa4001890] (size: 1024 bytes)
    Thread 1 (TID: 53186) - Malloc #4: [0xffffa40018a0 - 0xffffa4001ca0] (size: 1024 bytes)
    Thread 1 (TID: 53186) - Malloc #5: [0xffffa4001cb0 - 0xffffa40020b0] (size: 1024 bytes)
ffffa4021000    65404        0        0 --- [ anon ]
---------------------------------------------------
---------------- Thread Arena - HEAP --------------
ffffac000000      132       16       16 rw- [ anon ]
    Thread 0 (TID: 53185) - Malloc #1: [0xffffac000c70 - 0xffffac001070] (size: 1024 bytes)
    Thread 0 (TID: 53185) - Malloc #2: [0xffffac001080 - 0xffffac001480] (size: 1024 bytes)
    Thread 0 (TID: 53185) - Malloc #3: [0xffffac001490 - 0xffffac001890] (size: 1024 bytes)
    Thread 0 (TID: 53185) - Malloc #4: [0xffffac0018a0 - 0xffffac001ca0] (size: 1024 bytes)
    Thread 0 (TID: 53185) - Malloc #5: [0xffffac001cb0 - 0xffffac0020b0] (size: 1024 bytes)
ffffac021000    65404        0        0 --- [ anon ]
---------------------------------------------------
ffffb2180000       64        0        0 --- [ anon ]
---------------- Thread 2 - STACK ---------------
ffffb2190000     8192        8        8 rw- [ anon ]
    Thread 2 (TID: 53187) - Stack: [0xffffb2190000 - 0xffffb2990000] (size: 8388608 bytes)
---------------------------------------------------
ffffb2990000       64        0        0 --- [ anon ]
---------------- Thread 1 - STACK ---------------
ffffb29a0000     8192        8        8 rw- [ anon ]
    Thread 1 (TID: 53186) - Stack: [0xffffb29a0000 - 0xffffb31a0000] (size: 8388608 bytes)
---------------------------------------------------
ffffb31a0000       64        0        0 --- [ anon ]
---------------- Thread 0 - STACK ---------------
ffffb31b0000     8192        8        8 rw- [ anon ]
    Thread 0 (TID: 53185) - Stack: [0xffffb31b0000 - 0xffffb39b0000] (size: 8388608 bytes)
---------------------------------------------------
ffffb39b0000     1568     1152        0 r-x libc.so.6
ffffb3b38000       60        0        0 --- libc.so.6
ffffb3b47000       16       16       16 r-- libc.so.6
ffffb3b4b000        8        8        8 rw- libc.so.6
ffffb3b4d000       48       24       24 rw- [ anon ]
ffffb3b62000      172      172        0 r-x ld-linux-aarch64.so.1
ffffb3b96000        8        8        8 rw- [ anon ]
ffffb3b98000        8        0        0 r-- [ anon ]
ffffb3b9a000        8        4        0 r-x [ anon ]
ffffb3b9c000        8        8        8 r-- ld-linux-aarch64.so.1
ffffb3b9e000        8        8        8 rw- ld-linux-aarch64.so.1
fffff9bfa000      132       12       12 rw- [ stack ]
```

### VSZ Usage via ps
As we now know which PID our program has we can check the VSZ it has with ps, as illustrated below. 
In our example here the test program has a VSZ of 223568 KB (223 MB), but only 1260 KB (1.2 MB) are really used. 
This sounds a lot for 3 threads with 5 malloc() allocations for 1024 bytes each, right?
```
dev@devcontainer:/workspaces/glibc-malloc-pthread-test$ ps -eo pid,tid,vsz,rss,comm
    PID     TID    VSZ   RSS COMMAND
      1       1   2304  1312 sh
     93      93   7000  2244 su
    611     611   4536  3016 bash
  11100   11100 223568  1260 glibcVSZPlaygro
  13534   13534   6828  2504 ps
```

### Virtual Memory Map via pmap
With the tool pmap we can take a look at the virtual memory for a process. 
Below you can see what it looks like for our example.
```
dev@devcontainer:/workspaces/glibc-malloc-pthread-test$ pmap -x 11100
11100:   ./glibcVSZPlayground -n3 -m 1024 -c 5
Address           Kbytes     RSS   Dirty Mode  Mapping
0000aaaad9190000      12       8       0 r-x-- glibcVSZPlayground
0000aaaad91a2000       4       4       4 r---- glibcVSZPlayground
0000aaaad91a3000       4       4       4 rw--- glibcVSZPlayground
0000aaab11e73000     132       4       4 rw---   [ anon ]
0000ffff98000000     132      16      16 rw---   [ anon ]
0000ffff98021000   65404       0       0 -----   [ anon ]
0000ffff9c000000     132      16      16 rw---   [ anon ]
0000ffff9c021000   65404       0       0 -----   [ anon ]
0000ffffa0000000     132      16      16 rw---   [ anon ]
0000ffffa0021000   65404       0       0 -----   [ anon ]
0000ffffa5c70000      64       0       0 -----   [ anon ]
0000ffffa5c80000    8192      16      16 rw---   [ anon ]
0000ffffa6480000      64       0       0 -----   [ anon ]
0000ffffa6490000    8192      16      16 rw---   [ anon ]
0000ffffa6c90000      64       0       0 -----   [ anon ]
0000ffffa6ca0000    8192      16      16 rw---   [ anon ]
0000ffffa74a0000    1568    1152       0 r-x-- libc.so.6
0000ffffa7628000      60       0       0 ----- libc.so.6
0000ffffa7637000      16      16      16 r---- libc.so.6
0000ffffa763b000       8       8       8 rw--- libc.so.6
0000ffffa763d000      48      24      24 rw---   [ anon ]
0000ffffa7659000     172     156       0 r-x-- ld-linux-aarch64.so.1
0000ffffa768e000       8       8       8 rw---   [ anon ]
0000ffffa7690000       8       0       0 r----   [ anon ]
0000ffffa7692000       4       4       0 r-x--   [ anon ]
0000ffffa7693000       8       8       8 r---- ld-linux-aarch64.so.1
0000ffffa7695000       8       8       8 rw--- ld-linux-aarch64.so.1
0000ffffc2035000     132      24      24 rw---   [ stack ]
---------------- ------- ------- -------
total kB          223568    1524     204
```

### Bringing the pmap and the Tool Output together
With the addresses printed for the stacks of the threads and also the addresses for the malloc allocations we can match the addresses to the pmap output.
Here is what this looks like for our example:
```
dev@devcontainer:/workspaces/glibc-malloc-pthread-test$ pmap -x 11100
11100:   ./glibcVSZPlayground -n3 -m 1024 -c 5
Address           Kbytes     RSS   Dirty Mode  Mapping
0000aaaad9190000      12       8       0 r-x-- glibcVSZPlayground
0000aaaad91a2000       4       4       4 r---- glibcVSZPlayground
0000aaaad91a3000       4       4       4 rw--- glibcVSZPlayground
0000aaab11e73000     132       4       4 rw---   [ anon ]  Main Arena - Heap for Main Thread
--------------------------------------------------------
0000ffff98000000     132      16      16 rw---   [ anon ]  Thread Arena - holds Heap for Thread 1
0000ffff98021000   65404       0       0 -----   [ anon ]
--------------------------------------------------------
0000ffff9c000000     132      16      16 rw---   [ anon ]  Thread Arena - holds Heap for Thread 2
0000ffff9c021000   65404       0       0 -----   [ anon ]
--------------------------------------------------------
0000ffffa0000000     132      16      16 rw---   [ anon ]  Thread Arena - holds Heap for Thread 3
0000ffffa0021000   65404       0       0 -----   [ anon ]
--------------------------------------------------------
0000ffffa5c70000      64       0       0 -----   [ anon ]
0000ffffa5c80000    8192      16      16 rw---   [ anon ]  Thread Stack for Thread 2
--------------------------------------------------------
0000ffffa6480000      64       0       0 -----   [ anon ]
0000ffffa6490000    8192      16      16 rw---   [ anon ]  Thread Stack for Thread 1
--------------------------------------------------------
0000ffffa6c90000      64       0       0 -----   [ anon ]
0000ffffa6ca0000    8192      16      16 rw---   [ anon ]  Thread Stack for Thread 0
--------------------------------------------------------
0000ffffa74a0000    1568    1152       0 r-x-- libc.so.6
0000ffffa7628000      60       0       0 ----- libc.so.6
0000ffffa7637000      16      16      16 r---- libc.so.6
0000ffffa763b000       8       8       8 rw--- libc.so.6
0000ffffa763d000      48      24      24 rw---   [ anon ]
0000ffffa7659000     172     156       0 r-x-- ld-linux-aarch64.so.1
0000ffffa768e000       8       8       8 rw---   [ anon ]
0000ffffa7690000       8       0       0 r----   [ anon ]
0000ffffa7692000       4       4       0 r-x--   [ anon ]
0000ffffa7693000       8       8       8 r---- ld-linux-aarch64.so.1
0000ffffa7695000       8       8       8 rw--- ld-linux-aarch64.so.1
--------------------------------------------------------
0000ffffc2035000     132      24      24 rw---   [ stack ]
```
As you can see we have:
- 3 Thread arenas with 64 MB each (192 MB), but only very little real usage (RSS column)
- 3 Thread stacks with 8 MB each (24 MB), but only very little real usage (RSS column)
--> Means of the 223 MB VSZ, nearly everything is related to the stack size and the thread arenas
