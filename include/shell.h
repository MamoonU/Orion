// shell.h - OS Shell Interface

#ifndef SHELL_H
#define SHELL_H
 
#define SHELL_ARGV_MAX  16              // max tokens on a single command line (command + arguments)
#define SHELL_LINE_MAX  256             // max length of a single input line
 
void shell_run(void);                   // Entry point - never returns: entry of a dedicated process
 
#endif