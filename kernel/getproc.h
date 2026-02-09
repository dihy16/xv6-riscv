// kernel/getproc.h
#ifndef GETPROC_H
#define GETPROC_H

#include "kernel/types.h"

enum procstate_u {
  U_UNUSED,
  U_USED,
  U_SLEEPING,
  U_RUNNABLE,
  U_RUNNING,
  U_ZOMBIE
};

struct procinfo {
  int pid;
  enum procstate_u state;
  uint64 sz;
  char name[16];
};

#endif
