#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
sieve(int read_fd) {
  int p;
  if (read(read_fd, &p, sizeof(int)) != sizeof(int)) {
    exit(0);
  }
  printf("prime %d\n", p);
  
  int n;
  if (read(read_fd, &n, sizeof(int)) != sizeof(int)) {
    exit(0);
  }

  int pipefd[2];
  pipe(pipefd);
  if (fork() > 0) {
    close(pipefd[0]);

    do {
      if (n % p > 0) {
        write(pipefd[1], &n, sizeof(int));
      }
    } while (read(read_fd, &n, sizeof(int)));
  
    close(pipefd[1]);
    wait((int *) 0);
    exit(0);
  } else {
    close(pipefd[1]);
    sieve(pipefd[0]);
  }
}

int
main(int argc, char *argv[])
{
  int pipefd[2];
  pipe(pipefd);

  if (fork() > 0) {
    // parent
    close(pipefd[0]);
    for (int i=2; i <= 35; i++) {
      write(pipefd[1], &i, sizeof(int));
    }
    close(pipefd[1]);
    wait((int *) 0);
  } else {
    // child
    close(pipefd[1]);
    sieve(pipefd[0]);
  }

  exit(0);
}
