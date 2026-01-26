#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pipefd[2];
  int pid, mypid;
  char recv[1];
  
  pipe(pipefd);
  pid = fork();
  if (pid > 0) {
    // parent
    write(pipefd[0], "a", 1);
    sleep(5);
    read(pipefd[1], recv, sizeof recv);
    mypid = getpid();
    printf("%d: received pong\n", mypid);
    wait((int *) 0);
  } else {
    // child
    read(pipefd[1], recv, 1);
    mypid = getpid();
    printf("%d: received ping\n", mypid);
    exit(0);
    write(pipefd[0], "b", 1);
  }

  exit(0);
}
