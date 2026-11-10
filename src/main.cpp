#include "grep_engine.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static bool is_directory(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool is_regular_file(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

static void collect_files_recursively(const std::string &dir_path, std::vector<std::string> &out_files) {
    DIR *dir = opendir(dir_path.c_str());
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string full_path = dir_path;
        if (!full_path.empty() && full_path[full_path.size() - 1] != '/') {
            full_path += '/';
        }
        full_path += name;

        if (is_directory(full_path)) {
            collect_files_recursively(full_path, out_files);
        } else if (is_regular_file(full_path)) {
            out_files.push_back(full_path);
        }
    }
    closedir(dir);
}

int main(int argc, char *argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    bool use_recursive_search = false;
    bool use_color = true;
    bool enable_profiling = false;
    std::string pattern_str;
    bool has_pattern = false;
    std::vector<std::string> target_paths;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-E") {
            if (i + 1 < argc) {
                pattern_str = argv[++i];
                has_pattern = true;
            } else {
                std::cerr << "Error: -E requires a pattern.\n";
                return 1;
            }
        } else if (arg == "-r" || arg == "-R") {
            use_recursive_search = true;
        } else if (arg.find("--color=") == 0) {
            std::string opt = arg.substr(8);
            use_color = (opt != "never");
        } else if (arg == "--profile") {
            enable_profiling = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [-r] [--color=auto|always|never] [--profile] -E <pattern> [file ...]\n";
            return 0;
        } else {
            target_paths.push_back(arg);
        }
    }

    if (!has_pattern) {
        std::cerr << "Error: Missing -E pattern.\n";
        return 1;
    }

    grep::Profiler profiler;
    grep::Regex *regex_obj = NULL;

    try {
        regex_obj = new grep::Regex(pattern_str);
    } catch (const grep::RegexError &e) {
        std::cerr << "Regex error: " << e.what() << "\n";
        return 1;
    }

    bool found_any = false;

    if (target_paths.empty()) {
        std::string line;
        while (std::getline(std::cin, line)) {
            grep::MatchInfo mi = regex_obj->find_all(line, enable_profiling ? &profiler : NULL);
            if (mi.found) {
                std::cout << grep::format_colored_line(line, mi, use_color) << "\n";
                found_any = true;
            }
        }
    } else {
        std::vector<std::string> files_to_search;
        for (size_t i = 0; i < target_paths.size(); ++i) {
            const std::string &path = target_paths[i];
            if (is_directory(path)) {
                if (use_recursive_search) {
                    collect_files_recursively(path, files_to_search);
                } else {
                    std::cerr << "grep: " << path << ": Is a directory\n";
                }
            } else if (is_regular_file(path)) {
                files_to_search.push_back(path);
            } else {
                std::cerr << "grep: " << path << ": No such file or directory\n";
            }
        }

        bool show_filename = (files_to_search.size() > 1 || use_recursive_search);

        for (size_t i = 0; i < files_to_search.size(); ++i) {
            const std::string &file_path = files_to_search[i];
            std::ifstream fin(file_path.c_str());
            if (!fin.is_open()) {
                std::cerr << "grep: " << file_path << ": Permission denied or unreadable\n";
                continue;
            }

            std::string line;
            while (std::getline(fin, line)) {
                grep::MatchInfo mi = regex_obj->find_all(line, enable_profiling ? &profiler : NULL);
                if (mi.found) {
                    if (show_filename) {
                        std::cout << "\033[35m" << file_path << "\033[36m:\033[0m";
                    }
                    std::cout << grep::format_colored_line(line, mi, use_color) << "\n";
                    found_any = true;
                }
            }
            fin.close();
        }
    }

    if (enable_profiling) {
        std::cerr << "\n[Regex Profiler Summary]\n"
                  << "  Lines processed       : " << profiler.lines_processed() << "\n"
                  << "  Total simulation steps : " << profiler.total_steps() << "\n"
                  << "  Total states visited  : " << profiler.total_states_visited() << "\n"
                  << "  Max active states      : " << profiler.max_active_states() << "\n";
    }

    delete regex_obj;
    return found_any ? 0 : 1;
}

