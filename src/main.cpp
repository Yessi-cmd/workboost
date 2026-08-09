#include "cli/cli.h"

#include <windows.h>

int main(int argc, char* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  return workboost::RunCli(argc, argv);
}
