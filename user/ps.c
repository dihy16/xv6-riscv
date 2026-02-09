#include "kernel/types.h"
#include "kernel/getproc.h"
#include "user/user.h"

char *states[] = {
  [U_UNUSED]   = "unused",
  [U_USED]     = "used",
  [U_SLEEPING] = "sleep",
  [U_RUNNABLE] = "runnable",
  [U_RUNNING]  = "run",
  [U_ZOMBIE]   = "zombie",
};

int
main(void)
{
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
