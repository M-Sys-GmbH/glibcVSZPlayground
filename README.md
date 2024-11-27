# Testing Program for glibc's pthreads and malloc()

In a project we had the task to investigate the high VSZ (Virtual Memory Size) usage on an embedded Linux system.
The software the client put on the embedded Linux device was highly multithreaded, it turned out that there were 2 reasons:
- pthread stack size for each thread
- thread arenas created by malloc() calls within the thread functions

## pthread Stack Size

## malloc() ThreadArenas


## Usage
```
Usage: ./glibcVSZPlayground [options]
Options:
  -n, --num-threads <num>                   Number of threads (default: 20)
  -s, --thread-stack-size <size>            Stack size for each thread in bytes (default: system default)
  -m, --malloc-sparse-inside-thread <size>Allocate memory inside each thread without initialization
  -f, --malloc-filled-inside-thread <size>Allocate memory inside each thread and fill with 0xAA
  -c, --count-of-mallocs <number>           Number of malloc calls inside each thread (default: 1)
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
- Stack addresses (start, end) and stack size for each thread
- malloc() calls, with size and address range of allocations
- PID of program

```
dev@devcontainer:/workspaces/glibc-malloc-pthread-test$ ./glibcVSZPlayground -n3 -m 1024 -c 5
Thread 0 started. TID: 11101, StackAddress: 0xffffa6ca0000, Stack End Address: 0xffffa74a0000, Stack Size: 8388608 bytes
Allocated 1024 bytes at address 0xffffa0000c70 to 0xffffa0001070
Allocated 1024 bytes at address 0xffffa0001080 to 0xffffa0001480
Allocated 1024 bytes at address 0xffffa0001490 to 0xffffa0001890
Allocated 1024 bytes at address 0xffffa00018a0 to 0xffffa0001ca0
Allocated 1024 bytes at address 0xffffa0001cb0 to 0xffffa00020b0
Thread 2 started. TID: 11103, StackAddress: 0xffffa5c80000, Stack End Address: 0xffffa6480000, Stack Size: 8388608 bytes
Allocated 1024 bytes at address 0xffff9c000c70 to 0xffff9c001070
Allocated 1024 bytes at address 0xffff9c001080 to 0xffff9c001480
Allocated 1024 bytes at address 0xffff9c001490 to 0xffff9c001890
Allocated 1024 bytes at address 0xffff9c0018a0 to 0xffff9c001ca0
Allocated 1024 bytes at address 0xffff9c001cb0 to 0xffff9c0020b0
Thread 1 started. TID: 11102, StackAddress: 0xffffa6490000, Stack End Address: 0xffffa6c90000, Stack Size: 8388608 bytes
Allocated 1024 bytes at address 0xffff98000c70 to 0xffff98001070
Allocated 1024 bytes at address 0xffff98001080 to 0xffff98001480
Allocated 1024 bytes at address 0xffff98001490 to 0xffff98001890
Allocated 1024 bytes at address 0xffff980018a0 to 0xffff98001ca0
Allocated 1024 bytes at address 0xffff98001cb0 to 0xffff980020b0
#################################
Program PID: 11100
Program running. Press Ctrl+C to exit.
#################################
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
