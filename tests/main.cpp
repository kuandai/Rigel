#include "TestFramework.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    auto& tests = Rigel::Test::registry();
    size_t failed = 0;
    size_t skipped = 0;
    bool listOnly = false;
    bool verbose = false;
    std::string filter;

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--filter" && i + 1 < args.size()) {
            filter = args[i + 1];
            ++i;
        } else if (arg.rfind("--filter=", 0) == 0) {
            filter = arg.substr(std::string("--filter=").size());
        }
    }

    if (listOnly) {
        for (const auto& test : tests) {
            std::cout << test.name << "\n";
        }
        return 0;
    }

    for (const auto& test : tests) {
        if (!filter.empty()) {
            std::string name(test.name);
            if (name.find(filter) == std::string::npos) {
                continue;
            }
        }
        try {
            test.fn();
            if (verbose) {
                std::cout << "[PASS] " << test.name << "\n";
            }
        } catch (const Rigel::Test::TestSkip& e) {
            ++skipped;
            std::cerr << "[SKIP] " << test.name << ": " << e.what() << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << "Ran " << tests.size() << " tests, " << failed << " failed, "
              << skipped << " skipped.\n";

    return failed == 0 ? 0 : 1;
}
