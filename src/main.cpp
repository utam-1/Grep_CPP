#include "grep_engine.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sys/stat.h>

static bool is_directory(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

static bool is_regular_file(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static void collect_files_recursively(const std::string &dir_path, std::vector<std::string> &out_files) {
    std::string search_path = dir_path;
    if (!search_path.empty() && search_path[search_path.size() - 1] != '\\' && search_path[search_path.size() - 1] != '/') {
        search_path += '\\';
    }
    std::string pattern = search_path + "*";

    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &find_data);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        std::string name = find_data.cFileName;
        if (name == "." || name == "..") continue;

        std::string full_path = search_path + name;
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            collect_files_recursively(full_path, out_files);
        } else {
            out_files.push_back(full_path);
        }
    } while (FindNextFileA(hFind, &find_data) != 0);

    FindClose(hFind);
}
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

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
#endif

static bool search_stream(
    std::istream &in,
    const grep::Regex &regex_obj,
    const std::string &filename_prefix,
    bool show_filename,
    bool use_color,
    grep::Profiler *profiler)
{
    bool found_any = false;
    std::string line;
    while (std::getline(in, line)) {
        grep::MatchInfo mi = regex_obj.find_all(line, profiler);
        if (mi.found) {
            if (show_filename) {
                if (use_color) {
                    std::cout << "\033[35m" << filename_prefix << "\033[36m:\033[0m";
                } else {
                    std::cout << filename_prefix << ':';
                }
            }
            std::cout << grep::format_colored_line(line, mi, use_color) << "\n";
            found_any = true;
        }
    }
    return found_any;
}

int main(int argc, char *argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    bool use_recursive_search = false;
    bool use_color = true;
    bool enable_profiling = false;
    bool end_of_options = false;
    std::string pattern_str;
    bool has_pattern = false;
    std::vector<std::string> target_paths;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (!end_of_options && arg == "--") {
            end_of_options = true;
        } else if (!end_of_options && arg == "-E") {
            if (i + 1 < argc) {
                pattern_str = argv[++i];
                has_pattern = true;
            } else {
                std::cerr << "Error: -E requires a pattern.\n";
                return 1;
            }
        } else if (!end_of_options && (arg == "-r" || arg == "-R")) {
            use_recursive_search = true;
        } else if (!end_of_options && arg.find("--color=") == 0) {
            std::string opt = arg.substr(8);
            use_color = (opt != "never");
        } else if (!end_of_options && arg == "--profile") {
            enable_profiling = true;
        } else if (!end_of_options && (arg == "-h" || arg == "--help")) {
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
    grep::Profiler *profiler_ptr = enable_profiling ? &profiler : NULL;
    bool found_any = false;

    try {
        grep::Regex regex_obj(pattern_str);

        if (target_paths.empty()) {
            if (use_recursive_search) {
                target_paths.push_back(".");
            } else {
                found_any = search_stream(std::cin, regex_obj, "", false, use_color, profiler_ptr);
            }
        }

        if (!target_paths.empty()) {
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

                if (search_stream(fin, regex_obj, file_path, show_filename, use_color, profiler_ptr)) {
                    found_any = true;
                }
                fin.close();
            }
        }
    } catch (const grep::RegexError &e) {
        std::cerr << "Regex error: " << e.what() << "\n";
        return 1;
    }

    if (enable_profiling) {
        std::cerr << "\n[Regex Profiler Summary]\n"
                  << "  Lines processed        : " << profiler.lines_processed() << "\n"
                  << "  Total simulation steps : " << profiler.total_steps() << "\n"
                  << "  Total states visited   : " << profiler.total_states_visited() << "\n"
                  << "  Max active states      : " << profiler.max_active_states() << "\n";
    }

    return found_any ? 0 : 1;
}
