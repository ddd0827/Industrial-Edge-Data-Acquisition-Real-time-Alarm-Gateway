#include <csignal>

#include "test_framework.h"

int main() {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    return egtest::run_all();
}
