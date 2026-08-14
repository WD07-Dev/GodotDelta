#include "cli.app.h"
#include<iostream>

int main(int argc, char **argv) {
    try {
        return CliApplication().run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "FINAL ERROR: " << exception.what() << "\n";
        return 1;
    }
}