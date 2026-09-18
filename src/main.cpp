#include "Cli.hpp"

int main(int argc, char** argv) {
    CliOptions options = parseCliOptions(argc, argv);
    return runCli(options);
}
