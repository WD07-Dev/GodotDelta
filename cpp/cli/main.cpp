#include "cli.app.h"
#include "core/common/path_utils.h"
#include<iostream>
#include<string>
#include<vector>

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
    std::vector<std::string> utf8_arguments;
    std::vector<char*> arguments;
    utf8_arguments.reserve(argc);
    arguments.reserve(argc);

    for(int index = 0; index < argc; ++index) {
        utf8_arguments.push_back(gddelta::common::path_to_utf8(std::filesystem::path(wide_argv[index])));
    }
    for(auto& argument : utf8_arguments) {
        arguments.push_back(argument.data());
    }

    try {
        return CliApplication().run(argc, arguments.data());
    } catch(const std::exception& exception) {
        std::cerr << "FINAL ERROR: " << exception.what() << "\n";
        return 1;
    }
}
#else
int main(int argc, char **argv) {
    try {
        return CliApplication().run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "FINAL ERROR: " << exception.what() << "\n";
        return 1;
    }
}
#endif