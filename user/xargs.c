#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

#define LINE_FEED '\n'
#define STRING_END '\0'
#define MAX_STRING 512

int
main(int argc, char *argv[])
{
  while (1) {
    int eof = 0;
    int char_index = 0;
    char *data = malloc(MAX_STRING);
    char *new_command[MAXARG];
    for (int i = 1; i < argc; i++) {
      new_command[i-1] = argv[i];
    }
    while (1) {
      char buf;
      int n = read(0, &buf, 1);
      if (n < 1) {
        eof = 1;
      }
      if (n < 1 || buf == LINE_FEED) {
        data[char_index] = STRING_END;
        break;
      }
      if (char_index >= MAX_STRING - 1) {
        break;
      }
      data[char_index++] = buf;
    }
    if (char_index == 0 && eof) {
      free(data);
      break;
    }
    new_command[argc - 1] = data;
    new_command[argc] = 0;
    if (fork() == 0) {
      exec(new_command[0], new_command);
      fprintf(2, "xargs: exec %s failed\n", new_command[0]);
      exit(1);
    } else {
      wait(0);
    }
    free(data);
  }
  exit(0);
}
