// orion.h - liborion: Orion OS user-space runtime

#ifndef ORION_H
#define ORION_H

#include <stdint.h>
#include <stddef.h>

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

// syscall primitive
int32_t syscall(uint32_t num, uint32_t a, uint32_t b, uint32_t c);

// process control
void    _exit  (int code);                          // terminate immediately
int     getpid (void);                              // return current PID
void    yield  (void);                              // voluntarily give up CPU
void    sleep  (uint32_t ticks);                    // sleep for N PIT ticks
int     wait   (int pid, int *exit_code);           // wait for child process
int     fork   (uint32_t child_entry);              // spawn child process at entry point
int     execve (const char *path);                  // replace current image with ELF at path

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
void    free   (void *ptr);                         // free pages

// output
int     putchar(int c);                             // write character
int     puts   (const char *s);                     // write string + newline
int     printf (const char *fmt, ...);              // formatted output
int     sprintf(char *buf, const char *fmt, ...);   // formatted output -> buffer

// string functions
size_t  strlen (const char *s);                                             // string length
char   *strcpy (char *dst, const char *src);                                // copy string
char   *strncpy(char *dst, const char *src, size_t n);                      // copy # characters between strings
int     strcmp (const char *a, const char *b);                              // compare two strings
int     strncmp(const char *a, const char *b, size_t n);                    // compare # characters between strings
char   *strchr (const char *s, int c);                                      // find character
char   *strcat (char *dst, const char *src);                                // concatenate
void   *memcpy (void *dst, const void *src, size_t n);                      // memory copy
void   *memset (void *dst, int c, size_t n);                                // memory fill
int     memcmp (const void *a, const void *b, size_t n);                    // memory compare

// number conversion
int     atoi   (const char *s);                                             // ascii -> integer
char   *itoa   (int value, char *buf, int base);                            // integer -> ascii


#endif
