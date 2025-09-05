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
    
    // For capture groups - track start and end of captures
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
    Epsilon = 299,
    BackRefStart = 300,
    Matched = 1000
};

// --- Global Counters ---
int next_capture_id = 1; // Start from 1 for capture groups (\1, \2, etc.)
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
            // Create capture group
            int current_capture_id = next_capture_id++;
            
            // Create start capture state
            auto start_capture = std::make_shared<State>();
            start_capture->c = Epsilon;
            start_capture->capture_start = current_capture_id;
            
            // Parse the inner expression
            Fragment inner_frag = parse(rx, parse_primary(rx), 0);
            
            if (rx.empty() || rx.front() != ')') {
                throw std::runtime_error("Expected ')' to close group");
            }
            rx.remove_prefix(1);
            
            // Create end capture state
            auto end_capture = std::make_shared<State>();
            end_capture->c = Epsilon;
            end_capture->capture_end = current_capture_id;
            
            // Connect: start_capture -> inner_frag -> end_capture
            start_capture->out = inner_frag.start;
            for (auto o : inner_frag.out) {
                *o = end_capture;
            }
            
            frag = { start_capture, { &end_capture->out } };
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
    next_capture_id = 1; // Reset for each regex

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

// --- NFA Simulation ---
struct CaptureInfo {
    std::map<int, std::string> groups; // capture_id -> captured string
    std::map<int, size_t> backref_pos; // For tracking position in backreference matching
};

using List = std::vector<std::pair<CaptureInfo, std::shared_ptr<State>>>;

bool ismatch(const List& l) {
    return std::any_of(l.begin(), l.end(), [](const auto& p) {
        return p.second->c == Matched;
    });
}

void addstate(std::shared_ptr<State> s, CaptureInfo cap_info, List& l) {
    if (!s || s->lastlist == listid) return;

    s->lastlist = listid;

    // Handle capture start/end
    if (s->capture_start >= 0) {
        cap_info.groups[s->capture_start] = ""; // Initialize empty capture
        cap_info.backref_pos[s->capture_start] = 0;
    }
    
    if (s->capture_end >= 0) {
        // Capture group ended - no special action needed here
        // The captured content is already in cap_info.groups
    }

    if (s->c == Split) {
        addstate(s->out, cap_info, l);
        addstate(s->out1, cap_info, l);
        return;
    } else if (s->c == Epsilon) {
        addstate(s->out, cap_info, l);
        return;
    }

    l.push_back({ cap_info, s });
}

void startlist(std::shared_ptr<State> s, List& l) {
    listid++;
    l.clear();
    CaptureInfo cap{};
    addstate(s, cap, l);
}

void match_step(List& cl, char c, List& nl) {
    listid++;
    nl.clear();

    for (auto [cap_info, s] : cl) {
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
                    
                    if (cap_info.groups.count(backref_id) && !cap_info.groups[backref_id].empty()) {
                        const std::string& captured = cap_info.groups[backref_id];
                        size_t pos = cap_info.backref_pos[backref_id];
                        
                        if (pos < captured.length() && captured[pos] == c) {
                            cap_info.backref_pos[backref_id]++;
                            
                            if (cap_info.backref_pos[backref_id] >= captured.length()) {
                                // Finished matching the backreference
                                cap_info.backref_pos[backref_id] = 0;
                                should_advance = true;
                            } else {
                                // Still matching backreference, stay in same state
                                // Update captures for any active groups
                                for (auto& [group_id, group_content] : cap_info.groups) {
                                    if (group_content.length() > 0) { // Only update if group is active
                                        // Check if we need to add this character to active captures
                                        // This is a simplified approach - in practice you'd track which groups are currently active
                                    }
                                }
                                addstate(s, cap_info, nl);
                                continue;
                            }
                        }
                    }
                }
                break;
        }
        
        if (should_advance) {
            // Add character to all currently active capture groups
            for (auto& [group_id, group_content] : cap_info.groups) {
                // Simple heuristic: if the group is initialized (exists in map), add character
                // This is simplified - a full implementation would track active capture state more precisely
                if (cap_info.groups.count(group_id)) {
                    cap_info.groups[group_id] += c;
                }
            }
            
            addstate(s->out, cap_info, nl);
        }
    }
}

int matchEpsilonNFA(std::shared_ptr<State> start, std::string_view text) {
    List cl, nl;

    bool match_from_start = false;
    listid++;
    CaptureInfo initial_cap{};
    
    if (start->c == MatchStart) {
        match_from_start = true;
        addstate(start->out, initial_cap, cl);
    } else {
        addstate(start, initial_cap, cl);
    }

    for (size_t text_idx = 0; text_idx <= text.length(); ++text_idx) {
        char current_char = (text_idx < text.length()) ? text[text_idx] : '\0';

        if (current_char == '\0') {
            List final_cl;
            for (auto [cap_info, s] : cl) {
                if (s->c == MatchEnd) {
                    addstate(s->out, cap_info, final_cl);
                } else {
                    final_cl.push_back({cap_info, s});
                }
            }
            if (ismatch(final_cl)) {
                return 1;
            }
        }

        if (!match_from_start && cl.empty()) {
            startlist(start, cl);
        } else if (text_idx == 0 && !match_from_start && cl.empty() && start->c != MatchStart) {
            startlist(start, cl);
        }

        if (!cl.empty()) {
            if (text_idx < text.length()) {
                match_step(cl, current_char, nl);
                std::swap(cl, nl);
            }
        }

        if (ismatch(cl)) {
            return 1;
        }

        if (cl.empty() && !match_from_start && text_idx < text.length()) {
            startlist(start, cl);
            match_step(cl, current_char, nl);
            std::swap(cl, nl);
            if (ismatch(cl)) return 1;
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