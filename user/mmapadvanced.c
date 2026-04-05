#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid;

    printf("=== Multi-region mmap test ===\n");

      // Map TWO different regions
        uint64 addr1 = mmap(PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 1);
          uint64 addr2 = mmap(2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 2);

            if(addr1 == 0 || addr2 == 0){
                printf("mmap failed\n");
                    exit(1);
                      }

                        printf("Mapped region1 at %p\n", (void*)addr1);
                          printf("Mapped region2 at %p\n", (void*)addr2);

                            int *r1 = (int*)addr1;
                              int *r2 = (int*)(addr2 + PGSIZE);

                                // Write different values
                                  *r1 = 10;
                                    *r2 = 20;

                                      printf("Parent wrote: r1=%d, r2=%d\n", *r1, *r2);

                                        pid = fork();
                                          if(pid < 0){
                                              printf("fork failed\n");
                                                  exit(1);
                                                    }

                                                      if(pid == 0){
                                                          // Child process

                                                              printf("Child read: r1=%d, r2=%d\n", *r1, *r2);

                                                                  // Modify both regions
                                                                      *r1 = 100;
                                                                          *r2 = 200;

                                                                              printf("Child wrote: r1=%d, r2=%d\n", *r1, *r2);

                                                                                  // Unmap ONLY region1
                                                                                      if(munmap(addr1) < 0)
                                                                                            printf("Child: munmap r1 failed\n");
                                                                                                else
                                                                                                      printf("Child: munmap r1 succeeded\n");

                                                                                                          // region2 should still be accessible
                                                                                                              printf("Child still sees r2=%d\n", *r2);

                                                                                                                  exit(0);
                                                                                                                    } else {
                                                                                                                        // Parent process
                                                                                                                            wait(0);

                                                                                                                                // Check both regions after child
                                                                                                                                    printf("Parent read after child: r1=%d, r2=%d\n", *r1, *r2);

                                                                                                                                        // Unmap both
                                                                                                                                            if(munmap(addr1) < 0)
                                                                                                                                                  printf("Parent: munmap r1 failed\n");
                                                                                                                                                      else
                                                                                                                                                            printf("Parent: munmap r1 succeeded\n");

                                                                                                                                                                if(munmap(addr2) < 0)
                                                                                                                                                                      printf("Parent: munmap r2 failed\n");
                                                                                                                                                                          else
                                                                                                                                                                                printf("Parent: munmap r2 succeeded\n");
                                                                                                                                                                                  }

                                                                                                                                                                                    printf("=== Test complete ===\n");
                                                                                                                                                                                      exit(0);
                                                                                                                                                                                      }
