#include "kernel/types.h"
#include "kernel/getproc.h"
#include "user/user.h"

int
main(void)
{
  char *states[] = {
    [0]   = "unused",
    [1]   = "used",
    [2]   = "sleep",
    [3]   = "runnable",
    [4]   = "run",
    [5]   = "zombie",
  };
  struct procinfo p[64];
  int n;

  n = getprocs(p, 64);
  if(n < 0){
    printf("ps: getprocs failed\n");
    exit(1);
  }

  printf("PID\tSTATE\tSIZE\tNAME\n");

  for(int i = 0; i < n; i++){
    printf("%d\t%s\t%ld\t%s\n",
           p[i].pid,
           states[p[i].state],
           p[i].sz,
           p[i].name);
  }

  exit(0);
}
