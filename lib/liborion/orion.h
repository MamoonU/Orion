// orion.h - liborion: Orion OS user-space runtime

#ifndef ORION_H
#define ORION_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// syscall numbers (from syscall.h)
#define SYS_YIELD       0
#define SYS_EXIT        1
#define SYS_GETPID      2
#define SYS_SLEEP       3
#define SYS_FORK        4
#define SYS_EXEC        5
#define SYS_WRITE       6
#define SYS_READ        7
#define SYS_OPEN        8
#define SYS_CLOSE       9
#define SYS_PIPE        10
#define SYS_DUP2        11
#define SYS_EXECVE      12
#define SYS_WAIT        13
#define SYS_CHDIR       14
#define SYS_GETCWD      15
#define SYS_READDIR     16
#define SYS_BIND        17
#define SYS_UNBIND      18
#define SYS_NSDUMP      19
#define SYS_MOUNT       20
#define SYS_SBRK        21
#define SYS_SIGNAL      22
#define SYS_SIGRETURN   23
#define SYS_KILL        24

// open flags (from vfs.h)
#define O_RDONLY    0x00
#define O_WRONLY    0x01
#define O_RDWR      0x02
#define O_CREAT     0x04
#define O_TRUNC     0x08
#define O_APPEND    0x10

// namespace bind flags (from namespace.h)
#define NS_BIND_REPLACE  0
#define NS_BIND_BEFORE   1
#define NS_BIND_AFTER    2

// standard file descriptors (UNIX convention)
#define STDIN_FILENO  0         // input
#define STDOUT_FILENO 1         // output
#define STDERR_FILENO 2         // error

// signal numbers
#define NSIG        32          // total signal slots
#define SIGHUP      1           // hangup (terminal closed)
#define SIGINT      2           // interrupt    (Ctrl+C)
#define SIGQUIT     3           // quit         (Ctrl+\)
#define SIGILL      4           // illegal instruction
#define SIGTRAP     5           // trace / breakpoint
#define SIGABRT     6           // abort()
#define SIGFPE      8           // floating-point exception
#define SIGKILL     9           // kill: cannot be caught or ignored
#define SIGUSR1     10          // user-defined 1
#define SIGSEGV     11          // segmentation fault
#define SIGUSR2     12          // user-defined 2
#define SIGPIPE     13          // write to broken pipe
#define SIGALRM     14          // alarm clock
#define SIGTERM     15          // termination request
#define SIGCHLD     17          // child process state changed
#define SIGCONT     18          // continue after stop
#define SIGSTOP     19          // stop: cannot be caught or ignored

typedef void (*sig_handler_t)(int);         // signal handler type and special values
extern void sigreturn_trampoline(void);     // defined in syscall.asm

#define SIG_DFL     ((sig_handler_t)0)      // default kernel action
#define SIG_IGN     ((sig_handler_t)1)      // ignore signal
#define SIG_ERR     ((sig_handler_t)-1)     // error return from signal()

// signal error numbers
extern int errno;

#define EPERM       1       // operation not permitted
#define ENOENT      2       // no such file or directory
#define ESRCH       3       // no such process
#define EINTR       4       // interrupted system call
#define EIO         5       // I/O error
#define EBADF       9       // bad file descriptor
#define ECHILD      10      // no child processes
#define ENOMEM      12      // out of memory
#define EACCES      13      // permission denied
#define EFAULT      14      // bad address
#define EBUSY       16      // device or resource busy
#define EEXIST      17      // file exists
#define ENODEV      19      // no such device
#define ENOTDIR     20      // not a directory
#define EISDIR      21      // is a directory
#define EINVAL      22      // invalid argument
#define EMFILE      24      // too many open files
#define ENOSPC      28      // no space left on device
#define EPIPE       32      // broken pipe
#define ENOSYS      38      // function not implemented

// syscall primitives
int32_t syscall(uint32_t num, uint32_t a, uint32_t b, uint32_t c);
int32_t syscall4(uint32_t num, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

// process control
void    _exit  (int code);                          // terminate immediately
int     getpid (void);                              // return current PID
void    yield  (void);                              // voluntarily give up CPU
void    sleep  (uint32_t ticks);                    // sleep for N PIT ticks
int     wait   (int pid, int *exit_code);           // wait for child process
int     fork   (uint32_t child_entry);              // spawn child process at entry point
int     execve (const char *path);                  // replace current image with ELF at path

// signals
sig_handler_t signal (int signum, sig_handler_t handler);   // register handler; returns old handler
int           kill   (int pid, int signum);                 // send signal to process
int           raise  (int signum);                          // send signal to self

// file I/O
int     open   (const char *path, int flags);                               // open
int     close  (int fd);                                                    // close
int     read   (int fd, void *buf, uint32_t len);                           // read
int     write  (int fd, const void *buf, uint32_t len);                     // write
int     pipe   (int pipefd[2]);                                             // create pipe ([0]=read end, [1]=write end)
int     dup2   (int oldfd, int newfd);                                      // duplicate fd
int     chdir  (const char *path);                                          // change working directory
int     getcwd (char *buf, uint32_t len);                                   // return working directory
int     readdir(int fd, uint32_t index, char *name_buf, uint32_t buflen);   // read directories by index

// namespace
int     bind   (const char *src, const char *dst, uint8_t flags);           // mount path -> another
int     unbind (const char *dst);                                           // remove binding
void    nsdump (void);                                                      // debug: print namespace
int     mount  (int srv_fd, const char *path, uint8_t flags);               // mount PULSAR server

// heap
void   *sbrk   (int increment);                     // extend process heap
void   *malloc (uint32_t size);                     // allocate new pages
void   *calloc (uint32_t nmemb, uint32_t size);     // allocate new pages + zero
void   *realloc(void *ptr, uint32_t new_size);      // resize existing allocation
void    free   (void *ptr);                         // free pages

// environment variables (FUTURE: bind to /env as files)
char   *getenv  (const char *name);                                     // return enviroment variable
int     setenv  (const char *name, const char *value, int overwrite);   // add/update enviroment variable
int     unsetenv(const char *name);                                     // remove enviroment variable

// output
int     putchar(int c);                                                     // write character
int     puts   (const char *s);                                             // write string + newline
int     printf (const char *fmt, ...);                                      // formatted output
int     sprintf(char *buf, const char *fmt, ...);                           // formatted output -> buffer
int     fprintf (int fd, const char *fmt, ...);                             // write formatted output to arbitrary fd (no FILE*)
int     vsnprintf(char *buf, uint32_t size, const char *fmt, va_list args); // output safely formatted va_list args
int     vprintf (const char *fmt, va_list args);                            // output va_list args 
int     snprintf(char *buf, uint32_t size, const char *fmt, ...);           // safely formatted output -> buffer

// string functions
size_t  strlen (const char *s);                                             // string length
size_t  strnlen (const char *s, size_t maxlen);                             // string length until # characters
char   *strcpy (char *dst, const char *src);                                // copy string
char   *strncpy(char *dst, const char *src, size_t n);                      // copy # characters between strings
int     strcmp (const char *a, const char *b);                              // compare two strings
int     strncmp(const char *a, const char *b, size_t n);                    // compare # characters between strings
char   *strchr (const char *s, int c);                                      // find character
char   *strcat (char *dst, const char *src);                                // concatenate
char   *strncat (char *dst, const char *src, size_t n);                     // concatenate until # characters
char   *strstr  (const char *haystack, const char *needle);                 // find first "needle" in "haystack"
char   *strtok  (char *str, const char *delim);                             // tokenise string
char   *strdup  (const char *s);                                            // duplicate string
char   *strerror(int errnum);                                               // error number -> string

// memory
void   *memcpy (void *dst, const void *src, size_t n);                      // memory copy
void   *memset (void *dst, int c, size_t n);                                // memory fill
int     memcmp (const void *a, const void *b, size_t n);                    // memory compare

// number conversion
int     atoi   (const char *s);                                             // ascii -> integer
char   *itoa   (int value, char *buf, int base);                            // integer -> ascii


#endif
