# Custom Grep Engine with NFA-based Regex Implementation

## Overview

This project implements a custom **grep-like text search utility** from scratch, featuring a complete **regular expression engine** built using **Non-deterministic Finite Automaton (NFA)** simulation. Unlike traditional grep implementations that rely on existing regex libraries, this engine constructs and simulates NFAs directly, providing deep insight into how regular expressions work under the hood.

## 🚀 Key Features

### Core Functionality
- **Complete Regex Engine**: Built from ground-up using Thompson's NFA construction algorithm
- **Grep-like Interface**: Familiar command-line interface similar to GNU grep
- **File and Directory Search**: Search individual files, multiple files, or entire directory trees
- **Standard Input Support**: Process text from pipes and standard input
- **Cross-platform**: Written in modern C++ with filesystem support

### Supported Regex Features
- **Literal Characters**: Basic character matching (`a`, `b`, `1`, etc.)
- **Special Characters**: 
  - `.` - matches any character
  - `^` - matches start of line/string
  - `$` - matches end of line/string
- **Character Classes**: 
  - `[abc]` - matches any character in set
  - `[^abc]` - matches any character NOT in set
- **Escape Sequences**:
  - `\d` - matches digits [0-9]
  - `\w` - matches word characters [a-zA-Z0-9_]
  - `\1`, `\2`, etc. - backreferences to capture groups
- **Quantifiers**:
  - `*` - zero or more occurrences
  - `+` - one or more occurrences
  - `?` - zero or one occurrence (optional)
- **Grouping**: `(pattern)` - creates numbered capture groups
- **Alternation**: `|` - logical OR between patterns

## 🏗️ Architecture

### NFA Construction (Thompson's Algorithm)
The regex engine uses **Thompson's construction algorithm** to convert regular expressions into NFAs:

1. **Parsing**: Recursive descent parser with operator precedence
2. **Fragment Assembly**: Build NFA incrementally using fragments
3. **State Connection**: Connect fragments based on regex operators
4. **Final Assembly**: Create complete NFA with accepting state

### NFA Simulation
The matching process uses **epsilon-NFA simulation**:

1. **Parallel State Tracking**: Maintain multiple active states simultaneously
2. **Epsilon Closure**: Handle transitions that don't consume input
3. **Character Processing**: Process each input character against all active states
4. **Capture Group Tracking**: Maintain capture group state for backreferences

### Key Components

```
├── NFAState              # Individual state in the automaton
├── NFAFragment           # Partially constructed NFA during parsing
├── CaptureGroupInfo      # Tracks capture group state during simulation
├── ActiveNFAState        # State + capture info during simulation
└── Main Engine Functions:
    ├── compile_regex_to_nfa()       # Convert regex string to NFA
    ├── parse_regex()                # Handle operator precedence
    ├── parse_primary_element()      # Parse basic regex elements
    └── match_text_with_nfa()        # Simulate NFA on input text
```

## 📖 Usage

### Basic Syntax
```bash
./grep_engine [-r] -E pattern [file ...]
```

### Options
- `-E pattern`: Extended regular expression pattern (required)
- `-r`: Recursive directory search
- `file ...`: Files to search (if none specified, reads from stdin)

### Examples

**Search for digits in a file:**
```bash
./grep_engine -E '\d+' input.txt
```

**Search for email patterns recursively:**
```bash
./grep_engine -r -E '\w+@\w+\.\w+' /path/to/directory
```

**Pipe input for processing:**
```bash
cat logfile.txt | ./grep_engine -E 'ERROR.*\d{4}'
```

**Use capture groups and backreferences:**
```bash
./grep_engine -E '(\w+)\s+\1' text.txt  # Find repeated words
```

## 🛠️ Building and Compilation

### Requirements
- C++17 compatible compiler (GCC 8+, Clang 7+, MSVC 2019+)
- Standard library with `<filesystem>` support

### Compilation
```bash
g++ -std=c++17 -O2 -o grep_engine grep_engine.cpp
```

## 📚 Educational Value

This implementation serves as an excellent learning resource for:

### Computer Science Concepts
- **Finite Automata Theory**: Practical implementation of NFAs
- **Compiler Design**: Parsing techniques and AST construction
- **Algorithm Design**: Thompson's construction and NFA simulation
- **Data Structures**: Graph representation and traversal

### Software Engineering Practices
- **Modern C++**: Smart pointers, RAII, STL containers
- **Code Organization**: Clear separation of parsing, construction, and simulation
- **Error Handling**: Robust error reporting and exception safety
- **Performance**: Efficient algorithms and memory management

## 🔍 Technical Deep Dive

### NFA vs DFA Approach
This implementation uses **NFA simulation** rather than **DFA construction** because:
- **Memory Efficiency**: NFAs are more compact, especially for complex patterns
- **Construction Speed**: Thompson's algorithm is linear in pattern size
- **Flexibility**: Easier to handle advanced features like backreferences
- **Educational Value**: Better demonstrates the underlying theory

### Capture Groups and Backreferences
The engine supports **numbered capture groups** with full **backreference** capability:
- Groups are created with parentheses: `(pattern)`
- Backreferences use `\1`, `\2`, etc.
- Capture state is tracked during simulation
- Supports nested and multiple capture groups

### Performance Characteristics
- **Time Complexity**: O(nm) where n = text length, m = NFA states
- **Space Complexity**: O(m) for NFA representation
- **Pattern Compilation**: O(p) where p = pattern length
- **Real-world Performance**: Suitable for moderate-sized files and patterns

## 🎯 Use Cases

### Development and Debugging
- **Log Analysis**: Search complex log patterns with capture groups
- **Code Analysis**: Find patterns in source code across projects
- **Data Processing**: Extract structured data using regex patterns
- **Testing**: Validate regex engines and compare implementations

### Educational Applications
- **CS Curriculum**: Demonstrate automata theory concepts
- **Regex Learning**: Understand how regex engines work internally
- **Algorithm Study**: Explore parsing and graph algorithms
- **Performance Analysis**: Compare different regex implementation strategies

## 🚧 Limitations and Future Enhancements

### Current Limitations
- **Unicode Support**: Currently handles ASCII characters only
- **Advanced Features**: No lookaheads, lookbehinds, or atomic groups
- **Performance**: Not optimized for extremely large files
- **POSIX Compliance**: Implements subset of full POSIX regex features

### Potential Enhancements
- **Unicode Support**: UTF-8/UTF-16 character handling
- **Performance Optimizations**: JIT compilation, DFA caching
- **Extended Features**: Lookarounds, non-greedy quantifiers
- **Better Error Messages**: More detailed syntax error reporting
- **Memory Optimization**: Reduce memory usage for large NFAs


