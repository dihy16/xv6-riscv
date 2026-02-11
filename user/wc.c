#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[512];

void
wc(int fd, char *name, int flag_l, int flag_w, int flag_c)
{
  int i, n;
  int l, w, c, inword;

  l = w = c = 0;
  inword = 0;
  while((n = read(fd, buf, sizeof(buf))) > 0){
    for(i=0; i<n; i++){
      c++;
      if(buf[i] == '\n')
        l++;
      if(strchr(" \r\t\n\v", buf[i]))
        inword = 0;
      else if(!inword){
        w++;
        inword = 1;
      }
    }
  }
  if(n < 0){
    printf("wc: read error\n");
    exit(1);
  }
  if (flag_l) printf("%d ", l);
  if (flag_w) printf("%d ", w);
  if (flag_c) printf("%d ", c);
  printf("%s\n", name);
}

int
main(int argc, char *argv[])
{
  int flag_l = 0;
  int flag_w = 0;
  int flag_c = 0;
  int fd, i;
  int files_start = argc;
  for(i = 1; i < argc; i++){
    if (argv[i][0] == '-') {
      if (strchr(argv[i], 'l')) flag_l = 1;
      if (strchr(argv[i], 'w')) flag_w = 1;
      if (strchr(argv[i], 'c')) flag_c = 1;
    }
    else {
      files_start = i;
      break;
    }
  }

  // if no flag argument, print all lines/words/characters 
  if (!flag_l && !flag_c && !flag_w) {
    flag_l = flag_c = flag_w = 1;
  }
  
  if(files_start == argc){
    wc(0, "", flag_l, flag_w, flag_c);
    exit(0);
  }

  for(i = files_start; i < argc; i++){
    if((fd = open(argv[i], O_RDONLY)) < 0){
      printf("wc: cannot open %s\n", argv[i]);
      exit(1);
    }
    wc(fd, argv[i], flag_l, flag_w, flag_c);
    close(fd);
  }
  exit(0);
}
