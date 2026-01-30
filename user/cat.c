#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[512];
int flag_line = 0;
int linecnt = 1;

int
readline(int fd, char *buf, int maxlen)
{
  int n;
  char c;
  int i = 0;

  while (i < maxlen - 1) {
    n = read(fd, &c, 1);
    if (n < 0)
      return -1;
    if (n == 0)
      break;

    buf[i++] = c;
    if (c == '\n')
      break;
  }

  if (i == 0)
    return 0;

  buf[i] = '\0';
  return i;
}


void
print_number(int n)
{
  char tmp[16];
  int i = 0;

  /* convert number to string (reverse) */
  while(n > 0){
    tmp[i++] = '0' + (n % 10);
    n /= 10;
  }

  for(int j = i; j < 6; j++)
    write(1, " ", 1);

  /* print digits in correct order */
  for(int j = i - 1; j >= 0; j--)
    write(1, &tmp[j], 1);

  write(1, "  ", 2);
}

void
cat(int fd)
{
  int n;
  while((n = readline(fd, buf, sizeof(buf))) > 0) {
    if (flag_line) {
      print_number(linecnt++);
    }
    if (write(1, buf, n) != n) {
      fprintf(2, "cat: write error\n");
      exit(1);
    }
  }
  if(n < 0){
    fprintf(2, "cat: read error\n");
    exit(1);
  }
}

int
main(int argc, char *argv[])
{
  int fd, i;
  int files_start = argc;
  for(i = 1; i < argc; i++){
    if (argv[i][0] == '-') {
      if (strchr(argv[i], 'n')) flag_line = 1;
    }
    else {
      files_start = i;
      break;
    }
  }
  if(files_start >= argc){
    cat(0);
    exit(0);
  }

  for(i = files_start; i < argc; i++){
    if((fd = open(argv[i], O_RDONLY)) < 0){
      fprintf(2, "cat: cannot open %s\n", argv[i]);
      exit(1);
    }
    cat(fd);
    close(fd);
  }
  exit(0);
}
