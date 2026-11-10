# Grep Engine Architecture & Library Guide

## Overview

**Grep Engine** is a lightweight, zero-dependency regular expression engine and text search utility written in portable C and C++. It constructs a Non-deterministic Finite Automaton (NFA) using Thompson's construction algorithm and simulates it on input text with support for submatches, capture groups, and backreferences.

---

## 🏛️ Architecture

The engine is structured into two core phases: **Compilation (NFA Construction)** and **Simulation (Matching)**.

```
Regex Pattern String
       │
       ▼
 [ Lexer / Parser ]  ──> Recursive descent parser with operator precedence
       │
       ▼
[ Thompson NFA ]     ──> Fragments with dangling output pointers connected
       │                 into a directed state graph with ε-transitions (OPCODE_SPLIT)
       ▼
[ NFA Simulation ]   ──> Parallel active state set + ε-closure step
       │                 Tracks capture spans [start, end) & backreference offsets
       ▼
  Match Result       ──> Exact match spans & capture groups returned to caller
```

### 1. NFA Construction (Thompson's Algorithm)
- **State Representation (`NFAState`)**: Each node represents a single character match (`char`), character class (`[abc]`, `[^abc]`), anchor (`^`, `$`), wildcard (`.`), shorthand (`\d`, `\w`, `\s`), backreference (`\1`..`\9`), or an $\epsilon$-transition split (`OPCODE_SPLIT`).
- **Fragments (`NFAFragment`)**: Represents partial subgraphs with unresolved forward pointers (`dangling_outputs`). As operators (`|`, `*`, `+`, `?`) and concatenation are parsed, dangling outputs are linked to downstream states without backtracking during construction.
- **Complexity**: Compilation runs in linear time $O(m)$ with respect to pattern length $m$.

### 2. $\epsilon$-NFA Simulation
- **Parallel Active States**: Maintains a set of currently active states (`ActiveNFAState`).
- **$\epsilon$-Closure**: Traverses split transitions and records capture group boundaries (`capture_group_start`, `capture_group_end`) without consuming characters.
- **Step Evaluation**: For each input character, only the states matching the character advance to the next state list.
- **Backreference Tracking**: Maintains current match offsets into previously captured slices to resolve backreferences like `(\w+)\s+\1`.

---

## 💡 Why This Engine Over Traditional C / C++ Regex?

| Feature / Characteristic | Standard C (`<regex.h>` / POSIX) | `std::regex` (C++11/14/17/20) | This Grep Engine (`grep_engine.h`) |
| :--- | :--- | :--- | :--- |
| **Portability across C/C++ Standards** | C POSIX only (unsupported on MSVC Windows without POSIX layer) | Requires C++11 or higher | **Universal**: Pure C89/C99/C11/C23 + C++98/C++11/C++17/C++20/C++23 |
| **Memory & Binary Footprint** | External platform libc dependency | Heavy template instantiations, bloated binary size | **Extremely lightweight**, zero external dependencies, fast compilation |
| **Catastrophic Backtracking (ReDoS)** | Varies by vendor implementation | Vulnerable (uses backtracking engines in GCC/Clang/MSVC) | **Guaranteed NFA parallelism**: avoids exponential backtracking traps on complex quantifiers |
| **Built-in Inspection & Profiler** | None | None | **Built-in execution metrics** (active states, step counters, states visited) |
| **API Usability** | Clunky `regcomp` / `regexec` with manual `regmatch_t` sizing | Verbose iterator and `std::smatch` mechanics | Simple, modern RAII wrapper for C++ and clean, robust C structs |

---

## 📖 Library Usage Guide

The library provides a single unified header: [`include/grep_engine.h`](file:///home/utam1/projects/Grep_CPP/include/grep_engine.h).

### 1. Using in C++ (Modern or Legacy C++98/C++03/C++11+)

```cpp
#include "grep_engine.h"
#include <iostream>

int main() {
    try {
        // Compile regex pattern
        grep::Regex re("(\\w+)@(\\w+\\.\\w+)");

        std::string line = "Reach us at contact@domain.com or dev@test.org";

        // Quick boolean check
        if (re.matches(line)) {
            std::cout << "Matches pattern!\n";
        }

        // Find all non-overlapping matches and extract captures
        grep::MatchInfo info = re.find_all(line);
        for (size_t i = 0; i < info.detailed_matches.size(); ++i) {
            const grep::SingleMatch &m = info.detailed_matches[i];
            std::cout << "Match [" << m.start_pos << ", " << m.end_pos << "): "
                      << line.substr(m.start_pos, m.end_pos - m.start_pos) << "\n";
            std::cout << "  User : " << m.captures[1].second << "\n";
            std::cout << "  Domain: " << m.captures[2].second << "\n";
        }

        // Format terminal output with ANSI colors
        std::cout << grep::format_colored_line(line, info) << "\n";

    } catch (const grep::RegexError &e) {
        std::cerr << "Regex compilation error: " << e.what() << "\n";
    }
    return 0;
}
```

### 2. Using in Pure C (C89 / C90 / C99 / C11 / C17 / C23)

```c
#include "grep_engine.h"
#include <stdio.h>

int main(void) {
    grep_regex_t *re = NULL;
    char err[128];
    grep_match_result_t result;
    const char *text = "hello hello world";

    /* Compile pattern */
    if (grep_compile("(\\w+)\\s+\\1", &re, err, sizeof(err)) != GREP_OK) {
        fprintf(stderr, "Compilation failed: %s\n", err);
        return 1;
    }

    /* Fast boolean match test */
    if (grep_match(re, text) == 1) {
        printf("Pattern matched!\n");
    }

    /* Find all matches and inspect capture groups */
    if (grep_find_all(re, text, &result, NULL) == GREP_OK) {
        size_t i;
        for (i = 0; i < result.match_count; ++i) {
            printf("Match %zu: [%zu, %zu)\n", i,
                   result.matches[i].start_pos,
                   result.matches[i].end_pos);

            if (result.matches[i].captures[1].valid) {
                printf("  Capture 1: [%zu, %zu)\n",
                       result.matches[i].captures[1].start_pos,
                       result.matches[i].captures[1].end_pos);
            }
        }
        grep_match_result_free(&result);
    }

    /* Free compiled regex */
    grep_free(re);
    return 0;
}
```

### 3. Profiling Performance

Both C and C++ APIs support optional profiler metrics to inspect automaton execution:

```cpp
grep::Profiler profiler;
grep::Regex re("a*b");
re.matches("aaaaaaaaab", &profiler);

std::cout << "Total Steps: " << profiler.total_steps() << "\n"
          << "States Visited: " << profiler.total_states_visited() << "\n"
          << "Max Active States: " << profiler.max_active_states() << "\n";
```

---

## 🛠️ CMake Integration

Add to your `CMakeLists.txt`:

```cmake
add_subdirectory(path/to/Grep_CPP)
target_link_libraries(your_target PRIVATE grep_engine)
```
