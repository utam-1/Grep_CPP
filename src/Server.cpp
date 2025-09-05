#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <string_view>

namespace fs = std::filesystem;

// --- NFA State Definition ---
struct State {
    std::shared_ptr<State> out, out1;
    int c = -1;
    int lastlist = -1;
    std::vector<int> match_set;
    
    // For capture groups
    int capture_start = -1;  // If >= 0, this state starts capture group N
    int capture_end = -1;    // If >= 0, this state ends capture group N
};

// --- NFA Fragment Definition ---
struct Fragment {
    std::shared_ptr<State> start;
    std::vector<std::shared_ptr<State>*> out;
};

// --- NFA Opcodes ---
enum {
    Split = 256,
    MatchAny,
    MatchWord,
    MatchDigit,
    MatchChoice,
    MatchAntiChoice,
    MatchStart,
    MatchEnd,
    BackRefStart = 300,
    Matched = 1000
};

// --- Global Counters ---
int next_capture_id = 1;
int listid = 0;

// --- Utility Functions ---
std::vector<std::string> find_files_recursively(fs::path dir) {
    std::vector<std::string> files;

    if (!fs::is_directory(dir)) {
        if (fs::is_regular_file(dir)) {
            return {dir.string()};
        }
        return {};
    }

    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!fs::is_directory(e)) {
            files.push_back(e.path().string());
        }
    }
    return files;
}

// Forward declaration
Fragment parse(std::string_view&, Fragment, int);

// --- NFA Construction ---
Fragment parse_primary(std::string_view& rx) {
    auto state = std::make_shared<State>();
    Fragment frag { state, { &state->out } };

    if (rx.empty()) {
        throw std::runtime_error("Unexpected end of pattern");
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
                state->c = BackRefStart + (c - '0');
            } else state->c = c;
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
            if (rx.empty()) throw std::runtime_error("Unclosed bracket expression");
            rx.remove_prefix(1);
            break;
        }
        case '(': {
            // Create capture group with proper start/end states
            int current_capture_id = next_capture_id++;
            
            // Create capture start state
            auto start_state = std::make_shared<State>();
            start_state->c = Split;  // Use Split as epsilon-like transition
            start_state->capture_start = current_capture_id;
            
            // Parse inner content
            Fragment inner = parse(rx, parse_primary(rx), 0);
            
            if (rx.empty() || rx.front() != ')') {
                throw std::runtime_error("Expected ')' to close group");
            }
            rx.remove_prefix(1);
            
            // Create capture end state
            auto end_state = std::make_shared<State>();
            end_state->c = Split;
            end_state->capture_end = current_capture_id;
            
            // Wire together: start_state -> inner -> end_state
            start_state->out = inner.start;
            for (auto o : inner.out) {
                *o = end_state;
            }
            
            frag = { start_state, { &end_state->out } };
            break;
        }
        default:
            state->c = c;
    }

    // Handle quantifiers
    if (!rx.empty()) {
        switch (rx.front()) {
            case '*': {
                auto s = std::make_shared<State>();
                s->c = Split;
                s->out = frag.start;
                
                for (auto o : frag.out) {
                    *o = s;
                }
                
                frag = { s, { &s->out1 } };
                rx.remove_prefix(1);
            } break;
            case '+': {
                auto s = std::make_shared<State>();
                s->c = Split;
                s->out = frag.start;
                
                for (auto o : frag.out) {
                    *o = s;
                }
                
                frag.out = { &s->out1 };
                rx.remove_prefix(1);
            } break;
            case '?': {
                auto s = std::make_shared<State>();
                s->c = Split;
                s->out = frag.start;
                
                frag.start = s;
                frag.out.push_back(&s->out1);
                rx.remove_prefix(1);
            } break;
        }
    }

    return frag;
}

Fragment parse(std::string_view& rx, Fragment lhs, int min_prec) {
    auto prec = [](char c) {
        if (c == '|') return 0;
        if (c == ']' || c == ')') return -1;
        return 1;
    };

    char look = !rx.empty() ? rx.front() : '\0';

    while (!rx.empty() && prec(look) >= min_prec) {
        char op = look;

        if (op == '|') {
            rx.remove_prefix(1);
        }

        Fragment rhs = parse_primary(rx);

        if (!rx.empty()) {
            look = rx.front();
        } else {
            look = '\0';
        }

        while (!rx.empty() && prec(look) > prec(op)) {
            rhs = parse(rx, rhs, prec(op) + 1);
            if (rx.empty()) break;
            look = rx.front();
        }

        if (op == '|') {
            auto s = std::make_shared<State>();
            s->c = Split;
            s->out = lhs.start;
            s->out1 = rhs.start;

            lhs.start = s;
            lhs.out.insert(lhs.out.end(), rhs.out.begin(), rhs.out.end());
        } else {
            for (auto o : lhs.out) {
                *o = rhs.start;
            }
            lhs.out = rhs.out;
        }

        if (rx.empty()) break;
        look = rx.front();
    }

    return lhs;
}

std::shared_ptr<State> regex2nfa(std::string_view rx_str) {
    auto matched = std::make_shared<State>();
    matched->c = Matched;

    if (rx_str.empty()) return matched;

    std::string_view current_rx = rx_str;
    next_capture_id = 1;

    Fragment frag = parse(current_rx, parse_primary(current_rx), 0);

    if (!current_rx.empty()) {
        if (current_rx.front() == ')') throw std::runtime_error("Unmatched ')'");
        if (current_rx.front() == ']') throw std::runtime_error("Unmatched ']'");
        throw std::runtime_error("Syntax error in regex");
    }

    for (auto o : frag.out) {
        *o = matched;
    }

    return frag.start;
}

// --- NFA Simulation with Captures ---
struct CaptureInfo {
    std::map<int, std::string> groups;
    std::map<int, int> backref_pos;  // Position in backreference matching
    
    bool operator<(const CaptureInfo& other) const {
        if (groups != other.groups) return groups < other.groups;
        return backref_pos < other.backref_pos;
    }
};

struct NFAState {
    std::shared_ptr<State> state;
    CaptureInfo captures;
    
    bool operator<(const NFAState& other) const {
        if (state != other.state) return state < other.state;
        return captures < other.captures;
    }
};

using List = std::vector<NFAState>;

bool ismatch(const List& l) {
    return std::any_of(l.begin(), l.end(), [](const NFAState& ns) {
        return ns.state->c == Matched;
    });
}

void addstate(std::shared_ptr<State> s, CaptureInfo cap_info, List& l, std::set<std::shared_ptr<State>>& visited) {
    if (!s || visited.count(s)) return;
    visited.insert(s);

    // Handle capture start/end
    if (s->capture_start >= 0) {
        cap_info.groups[s->capture_start] = "";
        cap_info.backref_pos[s->capture_start] = 0;
    }
    
    if (s->capture_end >= 0) {
        // End of capture - the content is already in the group
    }

    if (s->c == Split) {
        addstate(s->out, cap_info, l, visited);
        addstate(s->out1, cap_info, l, visited);
        return;
    }

    l.push_back({ s, cap_info });
}

void startlist(std::shared_ptr<State> start, List& l) {
    l.clear();
    std::set<std::shared_ptr<State>> visited;
    CaptureInfo cap{};
    addstate(start, cap, l, visited);
}

void match_step(List& cl, char c, List& nl, const std::string& full_text, size_t pos) {
    nl.clear();
    
    for (const NFAState& ns : cl) {
        auto s = ns.state;
        auto cap_info = ns.captures;
        bool should_advance = false;
        
        switch (s->c) {
            case MatchAny:
                should_advance = true;
                break;
            case MatchDigit:
                should_advance = isdigit(c);
                break;
            case MatchWord:
                should_advance = (isalnum(c) || c == '_');
                break;
            case MatchChoice:
                should_advance = (std::find(s->match_set.begin(), s->match_set.end(), c) != s->match_set.end());
                break;
            case MatchAntiChoice:
                should_advance = (std::find(s->match_set.begin(), s->match_set.end(), c) == s->match_set.end());
                break;
            default:
                if (s->c == c) {
                    should_advance = true;
                } else if (s->c >= BackRefStart) {
                    // Handle backreference
                    int backref_id = s->c - BackRefStart;
                    
                    if (cap_info.groups.count(backref_id)) {
                        const std::string& captured = cap_info.groups[backref_id];
                        int pos_in_ref = cap_info.backref_pos[backref_id];
                        
                        if (!captured.empty() && pos_in_ref < captured.length() && captured[pos_in_ref] == c) {
                            cap_info.backref_pos[backref_id]++;
                            
                            if (cap_info.backref_pos[backref_id] >= captured.length()) {
                                // Finished matching backreference
                                cap_info.backref_pos[backref_id] = 0;
                                should_advance = true;
                            } else {
                                // Continue matching backreference
                                // Add character to active captures
                                for (auto& [gid, content] : cap_info.groups) {
                                    // Only add to groups that we're currently inside
                                    // This is a simplification - we should track active groups properly
                                    if (gid != backref_id) {  // Don't modify the group we're referencing
                                        cap_info.groups[gid] += c;
                                    }
                                }
                                nl.push_back({ s, cap_info });
                                continue;
                            }
                        }
                    }
                }
                break;
        }
        
        if (should_advance) {
            // Add character to all active capture groups
            // This is simplified - we should track which groups are currently active
            for (auto& [group_id, content] : cap_info.groups) {
                // Add to groups that exist and are presumably active
                cap_info.groups[group_id] += c;
            }
            
            std::set<std::shared_ptr<State>> visited;
            addstate(s->out, cap_info, nl, visited);
        }
    }
}

int matchEpsilonNFA(std::shared_ptr<State> start, std::string_view text) {
    List cl, nl;
    
    bool match_from_start = false;
    if (start->c == MatchStart) {
        match_from_start = true;
        std::set<std::shared_ptr<State>> visited;
        CaptureInfo initial_cap{};
        addstate(start->out, initial_cap, cl, visited);
    } else {
        startlist(start, cl);
    }

    std::string text_str(text);  // Convert to string for easier handling
    
    for (size_t text_idx = 0; text_idx <= text.length(); ++text_idx) {
        char current_char = (text_idx < text.length()) ? text[text_idx] : '\0';

        // Handle end anchor
        if (current_char == '\0') {
            List final_cl;
            for (const NFAState& ns : cl) {
                if (ns.state->c == MatchEnd) {
                    std::set<std::shared_ptr<State>> visited;
                    addstate(ns.state->out, ns.captures, final_cl, visited);
                } else {
                    final_cl.push_back(ns);
                }
            }
            if (ismatch(final_cl)) {
                return 1;
            }
        }

        // Restart matching if needed
        if (!match_from_start && cl.empty() && text_idx < text.length()) {
            startlist(start, cl);
        }

        if (!cl.empty() && text_idx < text.length()) {
            match_step(cl, current_char, nl, text_str, text_idx);
            std::swap(cl, nl);
        }

        if (ismatch(cl)) {
            return 1;
        }

        // Try starting new match from current position
        if (cl.empty() && !match_from_start && text_idx < text.length()) {
            startlist(start, cl);
            if (!cl.empty()) {
                match_step(cl, current_char, nl, text_str, text_idx);
                std::swap(cl, nl);
                if (ismatch(cl)) return 1;
            }
        }
    }

    return 0;
}

// --- Main Program ---
int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [-r] -E pattern [file ...]" << std::endl;
        return 1;
    }

    bool foundE = false;
    bool recursive = false;
    std::string pattern_str;
    std::vector<std::string> target_paths;

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
        std::cerr << "Error: Expected -E followed by a pattern.\n";
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

    if (target_paths.empty()) {
        if (recursive) {
            target_paths.push_back(".");
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
        for (const auto& target : target_paths) {
            if (fs::is_regular_file(target)) {
                files_to_process.push_back(target);
            } else if (fs::exists(target)) {
                std::cerr << "Warning: Skipping non-regular file: " << target << std::endl;
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

    return !any_match_found_global;
}