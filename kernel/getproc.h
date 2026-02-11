// kernel/getproc.h
#ifndef GETPROC_H
#define GETPROC_H

// Structure to hold process information
struct procinfo {
  int pid;                // Process ID
  int state;   // Process state
  uint64 sz;              // Size of process memory (bytes)
  char name[16];          // Process name
};

#endif
