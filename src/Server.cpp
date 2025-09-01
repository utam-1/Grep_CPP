#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <string_view> // Use string_view for efficiency

namespace fs = std::filesystem;

// --- NFA State Definition ---
struct State {
    std::shared_ptr<State> out, out1; // out: next state, out1: alternative next state (for Split)
    int c = -1;                       // character or special opcode
    int lastlist = -1;                // For list deduplication in simulation
    std::vector<int> match_set;       // For bracket expressions [abc]
    std::vector<int> capture_ids;     // IDs of capture groups this state belongs to
};

// --- NFA Fragment Definition ---
// A piece of an NFA graph, with a start state and a list of dangling
// outgoing pointers that need to be connected to subsequent parts.
struct Fragment {
    std::shared_ptr<State> start;
    std::vector<std::shared_ptr<State>*> out; // Pointers to shared_ptr<State> to be linked
};

// --- NFA Opcodes ---
// Special values for State::c to represent regex operators
enum {
    Split = 256,    // Represents an epsilon transition with two choices (e.g., for | or *)
    MatchAny,       // .
    MatchWord,      // \w
    MatchDigit,     // \d
    MatchChoice,    // [abc]
    MatchAntiChoice,// [^abc]
    MatchStart,     // ^
    MatchEnd,       // $
    Epsilon = 299,  // A pure epsilon transition (not strictly used as a char, but concept)
    BackRefStart = 300, // Starting point for backreference IDs (e.g., \1 is BackRefStart + 1)
    Matched = 1000  // Special state indicating a successful match
};

// --- Global Counters ---
int capture_id_counter = 0; // Assigns unique IDs to capture groups during parsing
int listid = 0;             // Incremented for each step of NFA simulation to prevent cycles

// --- Utility: Recursively find files in a directory ---
std::vector<std::string> find_files_recursively(fs::path dir) {
    std::vector<std::string> files;

    if (!fs::is_directory(dir)) {
        // If it's a file, just return it
        if (fs::is_regular_file(dir)) {
            return {dir.string()};
        }
        return {}; // Not a directory and not a file
    }

    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!fs::is_directory(e)) {
            files.push_back(e.path().string());
        }
    }
    return files;
}

// --- NFA Construction Helpers ---

// `capture_fragment` marks all states in a fragment as belonging to a capture group.
void capture_fragment(std::shared_ptr<State> s) {
    // This function recursively traverses the NFA fragment and adds the current
    // capture_id_counter to each state's capture_ids vector.
    // This is crucial for tracking which parts of the input text belong to which capture group.
    std::vector<std::shared_ptr<State>> q;
    q.push_back(s);
    std::set<std::shared_ptr<State>> visited; // Prevent infinite loops in cyclic NFAs

    while (!q.empty()) {
        std::shared_ptr<State> current = q.back();
        q.pop_back();

        if (!current || visited.count(current)) continue;
        visited.insert(current);

        // If this state hasn't been marked for this capture ID, add it
        if (std::find(current->capture_ids.begin(), current->capture_ids.end(), capture_id_counter) == current->capture_ids.end()) {
            current->capture_ids.push_back(capture_id_counter);
        }

        // Add next states to the queue for traversal
        if (current->out) {
            q.push_back(current->out);
        }
        if (current->out1) { // For Split states
            q.push_back(current->out1);
        }
    }
}


// Forward declaration for recursive parsing
Fragment parse(std::string_view&, Fragment, int);

// `parse_primary` handles single characters, '.', '\d', '\w', '[]', and parenthesized groups.
Fragment parse_primary(std::string_view& rx) {
    auto state = std::make_shared<State>();
    Fragment frag { state, { &state->out } };

    if (rx.empty()) {
        throw std::runtime_error("Unexpected end of pattern (expected primary expression)");
    }

    char c = rx.front();
    rx.remove_prefix(1);

    switch (c) {
        case '.':
            state->c = MatchAny;
            break;
        case '^':
            state->c = MatchStart;
            break;
        case '$':
            state->c = MatchEnd;
            break;
        case '\\': {
            if (rx.empty()) throw std::runtime_error("Unexpected end of pattern after \\");
            c = rx.front();
            rx.remove_prefix(1);
            if (c == 'd') state->c = MatchDigit;
            else if (c == 'w') state->c = MatchWord;
            else if (isdigit(c)) {
                // Backreference: \1, \2, etc.
                state->c = BackRefStart + (c - '0');
            } else state->c = c; // Escaped literal character (e.g., \.)
            break;
        }
        case '[': {
            bool neg = false;
            if (!rx.empty() && rx.front() == '^') {
                neg = true;
                rx.remove_prefix(1);
            }
            state->c = neg ? MatchAntiChoice : MatchChoice;

            while (!rx.empty() && rx.front() != ']') {
                state->match_set.push_back(rx.front());
                rx.remove_prefix(1);
            }
            if (rx.empty()) throw std::runtime_error("Unclosed bracket expression (missing ']')");
            rx.remove_prefix(1); // Consume ']'
            break;
        }
        case '(': {
            // This is a nested group. Recursively parse its content.
            // Note: The `parse` function handles `|` inside the group correctly.
            frag = parse(rx, parse_primary(rx), 0); // Parse inner expression
            
            if (rx.empty() || rx.front() != ')') {
                throw std::runtime_error("Expected ')' to close group");
            }
            rx.remove_prefix(1); // Consume ')'

            // Once the group is parsed, mark its states for the current capture ID
            // and increment the counter for the next group.
            capture_fragment(frag.start);
            capture_id_counter++;
            break;
        }
        default:
            state->c = c; // Literal character
    }

    // Handle postfix quantifiers: *, +, ?
    if (!rx.empty()) {
        switch (rx.front()) {
            case '*': {
                auto s = std::make_shared<State>();
                s->c = Split;
                s->out = frag.start; // Loop back to the start of the fragment

                for (auto o : frag.out) {
                    *o = s; // Connect fragment's end to the loop back state
                }

                frag = { s, { &s->out1 } }; // New fragment starts with Split, output to next pattern or bypass loop
                rx.remove_prefix(1);
            } break;
            case '+': {
                auto s = std::make_shared<State>();
                s->c = Split;
                s->out = frag.start; // Loop back to the start of the fragment (at least once)

                for (auto o : frag.out) {
                    *o = s; // Connect fragment's end to the loop back state
                }

                frag.out = { &s->out1 }; // New fragment's output is the bypass
                rx.remove_prefix(1);
            } break;
            case '?': {
                auto s = std::make_shared<State>();
                s->c = Split;
                s->out = frag.start; // Optional: can bypass the fragment

                frag.start = s; // New start state is the Split
                frag.out.push_back(&s->out1); // Add a path through the fragment
                rx.remove_prefix(1);
            } break;
        }
    }

    return frag;
}

// `parse` implements operator precedence parsing (similar to shunting-yard)
// It takes a left-hand side fragment and combines it with subsequent operators and right-hand side fragments.
Fragment parse(std::string_view& rx, Fragment lhs, int min_prec) {
    // Defines precedence for operators. Higher number means tighter binding.
    // ')' and ']' have low/negative precedence to act as delimiters.
    auto prec = [](char c) {
        if (c == '|') return 0;
        if (c == ']' || c == ')') return -1; // Delimiters, effectively stop parsing
        return 1; // Concatenation (implicit) has higher precedence than |
    };

    char look = !rx.empty() ? rx.front() : '\0'; // Lookahead character

    while (!rx.empty() && prec(look) >= min_prec) {
        char op = look;

        if (op == '|') {
            rx.remove_prefix(1); // Consume '|'
        }

        Fragment rhs = parse_primary(rx); // Parse the right-hand side operand

        if (!rx.empty()) {
            look = rx.front();
        } else {
            look = '\0'; // End of pattern
        }

        // Handle higher precedence operators on the RHS (e.g., `ab|c` should parse `ab` first)
        while (!rx.empty() && prec(look) > prec(op)) {
            rhs = parse(rx, rhs, prec(op) + 1);
            if (rx.empty()) break;
            look = rx.front();
        }

        if (op == '|') {
            // Union (OR) operator: Create a new Split state.
            // One branch goes to LHS, other to RHS. Output is union of both.
            auto s = std::make_shared<State>();
            s->c = Split;
            s->out = lhs.start;
            s->out1 = rhs.start;

            lhs.start = s;
            lhs.out.insert(lhs.out.end(), rhs.out.begin(), rhs.out.end());
        } else {
            // Concatenation (implicit operator): Connect LHS's dangling outputs to RHS's start.
            for (auto o : lhs.out) {
                *o = rhs.start;
            }
            lhs.out = rhs.out; // New dangling outputs are RHS's dangling outputs
        }

        if (rx.empty()) break; // End of pattern
        look = rx.front(); // Update lookahead for next iteration
    }

    return lhs;
}

// `regex2nfa` is the entry point for converting a regex string to an NFA.
std::shared_ptr<State> regex2nfa(std::string_view rx_str) {
    auto matched = std::make_shared<State>();
    matched->c = Matched;

    if (rx_str.empty()) return matched;

    std::string_view current_rx = rx_str; // Use string_view for parsing
    capture_id_counter = 0; // Reset capture counter for each new regex

    // Start parsing, then connect the final NFA outputs to the 'Matched' state.
    Fragment frag = parse(current_rx, parse_primary(current_rx), 0);

    // If there's still unparsed regex, it's an error (e.g., unmatched ')' or ']')
    if (!current_rx.empty()) {
         if (current_rx.front() == ')') throw std::runtime_error("Unmatched ')' in regex");
         if (current_rx.front() == ']') throw std::runtime_error("Unmatched ']' in regex");
         throw std::runtime_error("Syntax error in regex pattern: " + std::string(current_rx));
    }

    for (auto o : frag.out) {
        *o = matched;
    }

    return frag.start;
}

// --- NFA Simulation Runtime ---

// `CaptureInfo` stores the captured substrings for each group.
struct CaptureInfo {
    std::vector<std::string> groups; // Stored by index (0-based)
    std::map<int, int> active;       // capture_id -> index in `groups`
    int backref_idx = 0;             // Current index for matching a backreference
};

// `List` represents the set of active NFA states at a given point in time.
using List = std::vector<std::pair<CaptureInfo, std::shared_ptr<State>>>;

// Checks if any state in the list is the 'Matched' state.
bool ismatch(const List& l) {
    return std::any_of(l.begin(), l.end(), [](const auto& p) {
        return p.second->c == Matched;
    });
}

// `addstate` adds a state and all states reachable via epsilon transitions
// to the current list, avoiding duplicates for the current `listid`.
void addstate(std::shared_ptr<State> s, CaptureInfo cap_info, List& l) {
    if (!s || s->lastlist == listid) return; // Already processed for this listid

    s->lastlist = listid; // Mark as visited for current list generation

    // Recursive epsilon closure
    if (s->c == Split) {
        addstate(s->out, cap_info, l);
        addstate(s->out1, cap_info, l);
        return;
    }

    l.push_back({ cap_info, s });
}

// Initializes the first list of active states from the NFA start state.
void startlist(std::shared_ptr<State> s, List& l) {
    listid++; // Increment global list ID
    l.clear();
    CaptureInfo cap{}; // Empty capture info for the start of a match
    addstate(s, cap, l);
}

// Appends a character to all relevant active capture groups for a given state.
void capture(char c, CaptureInfo& caps, std::shared_ptr<State> s) {
    for (int id : s->capture_ids) {
        // If this capture ID isn't active yet, initialize it
        if (!caps.active.count(id)) {
            caps.active[id] = caps.groups.size();
            caps.groups.emplace_back(); // Add a new empty string for this group
        }
        // Append the current character to the captured string
        caps.groups[caps.active[id]] += c;
    }
}

// `match_step` advances the NFA simulation for a single input character `c`.
// It takes the current list of active states (`cl`) and populates `nl` (next list).
void match_step(List& cl, char c, List& nl) {
    listid++; // Increment global list ID for the new `nl`
    nl.clear();

    for (auto [cap_info, s] : cl) {
        switch (s->c) {
            case MatchAny:
                capture(c, cap_info, s);
                addstate(s->out, cap_info, nl);
                break;
            case MatchDigit:
                if (isdigit(c)) {
                    capture(c, cap_info, s);
                    addstate(s->out, cap_info, nl);
                }
                break;
            case MatchWord:
                if (isalnum(c) || c == '_') {
                    capture(c, cap_info, s);
                    addstate(s->out, cap_info, nl);
                }
                break;
            case MatchChoice:
                if (std::find(s->match_set.begin(), s->match_set.end(), c) != s->match_set.end()) {
                    capture(c, cap_info, s);
                    addstate(s->out, cap_info, nl);
                }
                break;
            case MatchAntiChoice:
                if (std::find(s->match_set.begin(), s->match_set.end(), c) == s->match_set.end()) {
                    capture(c, cap_info, s);
                    addstate(s->out, cap_info, nl);
                }
                break;
            default:
                // Handle literal characters and backreferences
                if (s->c == c) {
                    capture(c, cap_info, s);
                    addstate(s->out, cap_info, nl);
                } else if (s->c >= BackRefStart && s->c < BackRefStart + capture_id_counter) {
                    // This is a backreference state (e.g., \1).
                    int backref_id = s->c - BackRefStart; // 0-indexed group ID for the regex parse
                    
                    // The capture groups are stored by their `active` mapping or directly by index
                    // If the group `backref_id` was captured, `backref_id` will be a key in `cap_info.active`
                    // and its value will be the 0-indexed position in `cap_info.groups`
                    if (cap_info.active.count(backref_id)) {
                        int group_idx = cap_info.active[backref_id];
                        const std::string& captured_str = cap_info.groups[group_idx];

                        if (!captured_str.empty() && cap_info.backref_idx < captured_str.length() &&
                            captured_str[cap_info.backref_idx] == c) {
                            
                            capture(c, cap_info, s); // Capture the current character
                            cap_info.backref_idx++; // Advance index in the backreference string

                            if (cap_info.backref_idx >= captured_str.length()) {
                                // Entire backreference matched, reset index and move to next state
                                cap_info.backref_idx = 0;
                                addstate(s->out, cap_info, nl);
                            } else {
                                // Still matching the backreference, stay in this state
                                addstate(s, cap_info, nl);
                            }
                        }
                    }
                }
        }
    }
}

// `matchEpsilonNFA` runs the full NFA simulation against the input text.
int matchEpsilonNFA(std::shared_ptr<State> start, std::string_view text) {
    List cl, nl; // Current list, next list

    // Initial setup: `^` anchor check
    bool match_from_start_of_string = false;
    listid++; // Increment listid for initial startlist
    CaptureInfo initial_cap{};
    // If the regex starts with '^', only add its next state.
    // Otherwise, add the regex start state normally.
    if (start->c == MatchStart) {
        match_from_start_of_string = true;
        addstate(start->out, initial_cap, cl); // Add state after '^'
    } else {
        addstate(start, initial_cap, cl); // Add the regex's start state
    }

    // `text_idx` tracks current position in `text`
    // `current_start_pos` tracks where the current regex match attempt began in `text`
    for (size_t text_idx = 0; text_idx <= text.length(); ++text_idx) {
        char current_char = (text_idx < text.length()) ? text[text_idx] : '\0'; // Use '\0' for end of string

        // Process MatchEnd ($) if we are at the end of the input string
        if (current_char == '\0') {
             List final_cl; // A temporary list to process MatchEnd states

             // Check if any active state *before* processing EOF can transition to MatchEnd
             for (auto [cap_info, s] : cl) {
                 if (s->c == MatchEnd) {
                     // Add the state *after* MatchEnd (which should be the Matched state)
                     // or the MatchEnd state itself if it's the terminal state
                     addstate(s->out, cap_info, final_cl);
                 } else {
                     // If it's not a MatchEnd state, it still carries its capture info
                     // for the final `ismatch` check, but no new char will be matched.
                     // It essentially 'dies' unless it's a MatchEnd.
                     final_cl.push_back({cap_info, s});
                 }
             }
             // After processing MatchEnd, check if we have a match
             if (ismatch(final_cl)) {
                 return 1;
             }
        }

        // If not anchored to start of string and `cl` is empty,
        // we can try starting a new match attempt from the current `text_idx`.
        if (!match_from_start_of_string && cl.empty()) {
            startlist(start, cl); // Reset `cl` to start a new match attempt
            // Re-process current char if a new match started from here
            // This is handled by the loop naturally for `text_idx < text.length()`
        } else if (text_idx == 0 && !match_from_start_of_string && cl.empty() && start->c != MatchStart) {
            // Special case for empty regex matching empty string at start
            startlist(start, cl);
        }

        // If we still have an active list after potential restart
        if (!cl.empty()) {
             // For the very last iteration (text_idx == text.length()),
             // `current_char` is `\0`. `match_step` won't find matches for `\0`
             // unless explicitly handled for literal `\0` or specific anchors.
             // We've already handled `$` (MatchEnd) explicitly.
             if (text_idx < text.length()) { // Don't call match_step for the past-the-end `\0`
                 match_step(cl, current_char, nl);
                 std::swap(cl, nl); // Move next list to current list
             }
        }

        // Check for match after processing the current character
        if (ismatch(cl)) {
            // If the regex does *not* end with $, a match can occur mid-string.
            // If it *does* end with $, we need to wait until the `text_idx == text.length()` iteration.
            // The NFA will naturally propagate to the `Matched` state only if it satisfies all conditions.
            // So, checking `ismatch(cl)` here is correct.
            return 1;
        }

        // If `cl` becomes empty and we're not anchored to the start,
        // and we haven't reached the end of the text, we should restart the NFA
        // from the start state, effectively trying to match from the next character.
        if (cl.empty() && !match_from_start_of_string && text_idx < text.length()) {
            // This restart logic is crucial for non-anchored matches (grep-like behavior)
            // It effectively means: if no path is currently active, try starting a new one
            // from the current character in the text.
            startlist(start, cl);
            // Re-process the current character with the fresh start state
            match_step(cl, current_char, nl);
            std::swap(cl, nl);
            if (ismatch(cl)) return 1;
        }
    }

    return 0; // No match found
}

// --- Main Program Logic ---

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc < 2) { // Minimum 2 arguments: program name and pattern (or -E pattern)
        std::cerr << "Usage: " << argv[0] << " [-r] -E pattern [file ...]" << std::endl;
        return 1;
    }

    bool foundE = false;
    bool recursive = false;
    std::string pattern_str;
    std::vector<std::string> target_paths;

    // Argument parsing logic (adapted from both snippets)
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-E") {
            foundE = true;
            if (++i < argc) {
                pattern_str = argv[i];
            } else {
                std::cerr << "Error: -E requires a pattern argument\n";
                return 1;
            }
        } else if (arg == "-r") {
            recursive = true;
        } else {
            target_paths.push_back(arg);
        }
    }

    if (!foundE) {
        std::cerr << "Error: Expected -E followed by a pattern. (e.g., -E \"pattern\")\n";
        return 1;
    }
    if (pattern_str.empty()) {
        std::cerr << "Error: Pattern cannot be empty.\n";
        return 1;
    }

    std::shared_ptr<State> nfa_start;
    try {
        nfa_start = regex2nfa(pattern_str);
    } catch (const std::runtime_error& e) {
        std::cerr << "Regex parsing error: " << e.what() << std::endl;
        return 1;
    }

    bool any_match_found_global = false;

    // If no target files/dirs are given, process stdin or current directory if recursive
    if (target_paths.empty()) {
        if (recursive) {
            target_paths.push_back("."); // Search current directory
        } else {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (matchEpsilonNFA(nfa_start, line)) {
                    std::cout << line << std::endl;
                    any_match_found_global = true;
                }
            }
            return any_match_found_global ? 0 : 1;
        }
    }

    std::vector<std::string> files_to_process;
    if (recursive) {
        for (const auto& target : target_paths) {
            auto found_files = find_files_recursively(target);
            files_to_process.insert(files_to_process.end(), found_files.begin(), found_files.end());
        }
    } else {
        // Just add regular files directly
        for (const auto& target : target_paths) {
            if (fs::is_regular_file(target)) {
                files_to_process.push_back(target);
            } else if (fs::exists(target)) {
                 std::cerr << "Warning: Skipping non-regular file/directory (not recursive): " << target << std::endl;
            } else {
                 std::cerr << "Error: Path not found: " << target << std::endl;
            }
        }
    }

    bool prefix_with_filename = (files_to_process.size() > 1);

    for (const auto& filepath_str : files_to_process) {
        std::ifstream fin(filepath_str);
        if (!fin.is_open()) {
            std::cerr << "Error: Could not open file " << filepath_str << std::endl;
            continue;
        }

        std::string line;
        while (std::getline(fin, line)) {
            // Re-generate NFA start for each line if needed, but for performance,
            // we build it once outside the loop. The `listid` mechanism
            // ensures clean state for each `matchEpsilonNFA` call.
            if (matchEpsilonNFA(nfa_start, line)) {
                if (prefix_with_filename) {
                    std::cout << filepath_str << ":";
                }
                std::cout << line << std::endl;
                any_match_found_global = true;
            }
        }
        fin.close();
    }

    return !any_match_found_global; // Return 0 if matches found, 1 otherwise
}