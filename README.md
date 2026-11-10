# Custom Grep Engine & Reusable Regex Library (C / C++)

A high-performance custom regular expression and grep search engine constructed from scratch using Thompson's NFA (Non-deterministic Finite Automaton) simulation.

Supports both a **C API (compatible with C89 / C90 / C99 / C11 / C17 / C23)** and a **C++ API (compatible with C++98 / C++03 / C++11 / C++17 / C++20 / C++23)**, making it easily embeddable in modern or legacy codebases without external dependencies.

---

## 🚀 Key Features

### 1. Regex Engine (NFA Construction & Simulation)
- Built using **Thompson's Construction Algorithm** and **$\epsilon$-NFA simulation**.
- **Literal Characters**: Exact character matches.
- **Wildcard**: `.` matches any character.
- **Line Anchors**: `^` (start of line), `$` (end of line).
- **Character Classes**:
  - `[abc]` - matches any character in set (supports ranges `[a-z]`, `[0-9]`).
  - `[^abc]` - negated character class (matches any character NOT in set).
- **Shorthand Classes**:
  - `\d` - digits `[0-9]`.
  - `\w` - word characters `[a-zA-Z0-9_]`.
  - `\s` - whitespace characters (spaces, tabs, newlines).
- **Quantifiers**:
  - `*` - zero or more occurrences.
  - `+` - one or more occurrences.
  - `?` - zero or one occurrence.
- **Grouping & Alternation**:
  - `(abc|def)` - sub-expressions and logical OR.
- **Numbered Capture Groups & Backreferences**:
  - `\1` through `\9` - backreference to text matched by capture groups.

### 2. Cross-Standard Portability & Reusability
- **Header**: Single unified header [`include/grep_engine.h`](file:///home/utam1/projects/Grep_CPP/include/grep_engine.h).
- **Pure C Interface**: `grep_compile`, `grep_match`, `grep_find_all`, `grep_free`.
- **C++ RAII Interface**: `grep::Regex`, `grep::MatchInfo`, `grep::Profiler`.
- **Zero External Dependencies**: Pure C/C++ runtime only.
- **Thread Safety**: Regex instances are immutable during matching and can be safely shared across threads.

---

## 📦 Building the Library and CLI

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest --output-on-failure
```

### Build Options
- `-DBUILD_SHARED_LIBS=ON|OFF` (Default: `OFF`, builds static library `libgrep_engine.a`)
- `-DGREP_BUILD_CLI=ON|OFF` (Default: `ON`, builds `grep` CLI utility)
- `-DGREP_BUILD_TESTS=ON|OFF` (Default: `ON`, builds unit test suite)

---

## 💻 Reusable Library Usage

### 1. In Pure C (C89 / C99 / C11 / C17 / C23)

```c
#include "grep_engine.h"
#include <stdio.h>

int main(void) {
    grep_regex_t *re = NULL;
    char err[128];
    grep_match_result_t res;

    if (grep_compile("(\\w+)\\s+\\1", &re, err, sizeof(err)) != GREP_OK) {
        fprintf(stderr, "Compile error: %s\n", err);
        return 1;
    }

    if (grep_find_all(re, "hello hello world", &res, NULL) == GREP_OK) {
        if (res.found) {
            printf("Found %zu matches!\n", res.match_count);
            printf("First match span: [%zu, %zu)\n",
                   res.matches[0].start_pos, res.matches[0].end_pos);
            if (res.matches[0].captures[1].valid) {
                printf("Capture group 1 span: [%zu, %zu)\n",
                       res.matches[0].captures[1].start_pos,
                       res.matches[0].captures[1].end_pos);
            }
        }
        grep_match_result_free(&res);
    }

    grep_free(re);
    return 0;
}
```

### 2. In C++ (C++98 / C++11 / C++17 / C++20 / C++23)

```cpp
#include "grep_engine.h"
#include <iostream>

int main() {
    try {
        grep::Regex re("(\\w+)@(\\w+\\.\\w+)");
        std::string text = "Contact us at support@example.com or info@domain.org";

        grep::MatchInfo mi = re.find_all(text);
        if (mi.found) {
            for (size_t i = 0; i < mi.detailed_matches.size(); ++i) {
                std::cout << "Match: " << text.substr(mi.detailed_matches[i].start_pos,
                                                     mi.detailed_matches[i].end_pos - mi.detailed_matches[i].start_pos) << "\n";
                std::cout << "  User: " << mi.detailed_matches[i].captures[1].second << "\n";
                std::cout << "  Host: " << mi.detailed_matches[i].captures[2].second << "\n";
            }
        }
    } catch (const grep::RegexError &e) {
        std::cerr << "Regex error: " << e.what() << "\n";
    }
    return 0;
}
```

---

## 🔍 CLI Utility Usage

```bash
# Basic match on standard input
echo "hello 123 world" | ./grep -E '\d+'

# Search file with colored match highlighting
./grep -E '\w+@\w+\.\w+' emails.txt

# Recursive directory search
./grep -r -E 'class [A-Za-z]+' src/

# Profiling NFA execution
./grep -E 'a*b' large_file.txt --profile
```

---

## 🧪 Testing

The test suite thoroughly verifies both the C API and C++ API across various standards:

```bash
cd build
ctest --output-on-failure
# Or run executables directly:
./test_c_api
./test_cpp_api
```
