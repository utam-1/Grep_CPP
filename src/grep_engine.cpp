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
#include <windows.h>
namespace fs = std::filesystem;

// ANSI color codes for terminal output
const std::string COLOR_RED_BOLD = "\033[1;31m";
const std::string COLOR_RESET = "\033[0m";

// --- NFA State Definition ---
struct NFAState {
    std::shared_ptr<NFAState> primary_transition, alternative_transition;
    int character_code = -1;
    int last_list_id = -1;
    std::vector<int> character_set;
    int capture_group_start = -1;
    int capture_group_end = -1;
};

// --- NFA Fragment Definition ---
struct NFAFragment {
    std::shared_ptr<NFAState> start_state;
    std::vector<std::shared_ptr<NFAState>*> dangling_outputs;
};

// --- NFA Opcodes ---
enum RegexOpcodes {
    OPCODE_SPLIT = 256,
    OPCODE_MATCH_ANY,
    OPCODE_MATCH_WORD,
    OPCODE_MATCH_DIGIT,
    OPCODE_MATCH_CHOICE,
    OPCODE_MATCH_ANTI_CHOICE,
    OPCODE_MATCH_START,
    OPCODE_MATCH_END,
    OPCODE_BACKREF_START = 300,
    OPCODE_MATCHED = 1000
};

// --- Global Counters ---
int next_capture_group_id = 1;

// --- Utility Functions ---
std::vector<std::string> find_all_files_recursively(fs::path directory_path) {
    std::vector<std::string> found_files;

    if (!fs::is_directory(directory_path)) {
        if (fs::is_regular_file(directory_path)) {
            return {directory_path.string()};
        }
        return {};
    }

    for (auto& entry : fs::recursive_directory_iterator(directory_path)) {
        if (!fs::is_directory(entry)) {
            found_files.push_back(entry.path().string());
        }
    }
    return found_files;
}

// Forward declaration
struct NFAFragment;
NFAFragment parse_regex(std::string_view&, NFAFragment, int);

// --- NFA Construction ---
NFAFragment parse_primary_element(std::string_view& regex_pattern) {
    if (regex_pattern.empty()) {
        throw std::runtime_error("Unexpected end of pattern");
    }

    auto new_state = std::make_shared<NFAState>();
    NFAFragment current_fragment { new_state, { &new_state->primary_transition } };

    char current_char = regex_pattern.front();
    regex_pattern.remove_prefix(1);

    switch (current_char) {
        case '.':
            new_state->character_code = OPCODE_MATCH_ANY;
            break;
        case '^':
            new_state->character_code = OPCODE_MATCH_START;
            break;
        case '$':
            new_state->character_code = OPCODE_MATCH_END;
            break;
        case '\\': {
            if (regex_pattern.empty()) throw std::runtime_error("Unexpected end of pattern after \\");
            current_char = regex_pattern.front();
            regex_pattern.remove_prefix(1);
            if (current_char == 'd') new_state->character_code = OPCODE_MATCH_DIGIT;
            else if (current_char == 'w') new_state->character_code = OPCODE_MATCH_WORD;
            else if (isdigit(static_cast<unsigned char>(current_char))) {
                new_state->character_code = OPCODE_BACKREF_START + (current_char - '0');
            } else new_state->character_code = current_char;
            break;
        }
        case '[': {
            bool is_negated = false;
            if (!regex_pattern.empty() && regex_pattern.front() == '^') {
                is_negated = true;
                regex_pattern.remove_prefix(1);
            }
            new_state->character_code = is_negated ? OPCODE_MATCH_ANTI_CHOICE : OPCODE_MATCH_CHOICE;

            while (!regex_pattern.empty() && regex_pattern.front() != ']') {
                new_state->character_set.push_back(regex_pattern.front());
                regex_pattern.remove_prefix(1);
            }
            if (regex_pattern.empty()) throw std::runtime_error("Unclosed bracket expression");
            regex_pattern.remove_prefix(1);
            break;
        }
        case '(': {
            int current_capture_group_id = next_capture_group_id++;

            auto capture_start_state = std::make_shared<NFAState>();
            capture_start_state->character_code = OPCODE_SPLIT;
            capture_start_state->capture_group_start = current_capture_group_id;

            NFAFragment inner_fragment = parse_regex(regex_pattern, parse_primary_element(regex_pattern), 0);

            if (regex_pattern.empty() || regex_pattern.front() != ')') {
                throw std::runtime_error("Expected ')' to close group");
            }
            regex_pattern.remove_prefix(1);

            auto capture_end_state = std::make_shared<NFAState>();
            capture_end_state->character_code = OPCODE_SPLIT;
            capture_end_state->capture_group_end = current_capture_group_id;

            capture_start_state->primary_transition = inner_fragment.start_state;
            for (auto dangling_output : inner_fragment.dangling_outputs) {
                *dangling_output = capture_end_state;
            }

            current_fragment = { capture_start_state, { &capture_end_state->primary_transition } };
            break;
        }
        default:
            new_state->character_code = current_char;
    }

    if (!regex_pattern.empty()) {
        switch (regex_pattern.front()) {
            case '*': {
                auto split_state = std::make_shared<NFAState>();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;

                for (auto dangling_output : current_fragment.dangling_outputs) {
                    *dangling_output = split_state;
                }

                current_fragment = { split_state, { &split_state->alternative_transition } };
                regex_pattern.remove_prefix(1);
            } break;
            case '+': {
                auto split_state = std::make_shared<NFAState>();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;

                for (auto dangling_output : current_fragment.dangling_outputs) {
                    *dangling_output = split_state;
                }

                current_fragment.dangling_outputs = { &split_state->alternative_transition };
                regex_pattern.remove_prefix(1);
            } break;
            case '?': {
                auto split_state = std::make_shared<NFAState>();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;

                current_fragment.start_state = split_state;
                current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
                regex_pattern.remove_prefix(1);
            } break;
        }
    }

    return current_fragment;
}

NFAFragment parse_regex(std::string_view& regex_pattern, NFAFragment left_fragment, int min_precedence) {
    auto get_precedence = [](char operator_char) {
        if (operator_char == '|') return 0;
        if (operator_char == ']' || operator_char == ')') return -1;
        return 1;
    };

    char lookahead_char = !regex_pattern.empty() ? regex_pattern.front() : '\0';

    while (!regex_pattern.empty() && get_precedence(lookahead_char) >= min_precedence) {
        char current_operator = lookahead_char;

        if (current_operator == '|') {
            regex_pattern.remove_prefix(1);
        }

        NFAFragment right_fragment = parse_primary_element(regex_pattern);

        if (!regex_pattern.empty()) {
            lookahead_char = regex_pattern.front();
        } else {
            lookahead_char = '\0';
        }

        while (!regex_pattern.empty() && get_precedence(lookahead_char) > get_precedence(current_operator)) {
            right_fragment = parse_regex(regex_pattern, right_fragment, get_precedence(current_operator) + 1);
            if (regex_pattern.empty()) break;
            lookahead_char = regex_pattern.front();
        }

        if (current_operator == '|') {
            auto split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = left_fragment.start_state;
            split_state->alternative_transition = right_fragment.start_state;

            left_fragment.start_state = split_state;
            left_fragment.dangling_outputs.insert(left_fragment.dangling_outputs.end(), 
                                                  right_fragment.dangling_outputs.begin(), 
                                                  right_fragment.dangling_outputs.end());
        } else {
            for (auto dangling_output : left_fragment.dangling_outputs) {
                *dangling_output = right_fragment.start_state;
            }
            left_fragment.dangling_outputs = right_fragment.dangling_outputs;
        }

        if (regex_pattern.empty()) break;
        lookahead_char = regex_pattern.front();
    }

    return left_fragment;
}

std::shared_ptr<NFAState> compile_regex_to_nfa(std::string_view regex_string) {
    auto matched_state = std::make_shared<NFAState>();
    matched_state->character_code = OPCODE_MATCHED;

    if (regex_string.empty()) return matched_state;

    std::string_view remaining_pattern = regex_string;
    next_capture_group_id = 1;

    NFAFragment complete_fragment = parse_regex(remaining_pattern, parse_primary_element(remaining_pattern), 0);

    if (!remaining_pattern.empty()) {
        if (remaining_pattern.front() == ')') throw std::runtime_error("Unmatched ')'");
        if (remaining_pattern.front() == ']') throw std::runtime_error("Unmatched ']'");
        throw std::runtime_error("Syntax error in regex");
    }

    for (auto dangling_output : complete_fragment.dangling_outputs) {
        *dangling_output = matched_state;
    }

    return complete_fragment.start_state;
}

// --- NFA Simulation with Captures ---
struct CaptureGroupInfo {
    std::map<int, std::string> captured_text;
    std::map<int, bool> is_actively_capturing;
    std::map<int, size_t> backreference_position;
    
    bool operator<(const CaptureGroupInfo& other) const {
        if (captured_text != other.captured_text) return captured_text < other.captured_text;
        if (is_actively_capturing != other.is_actively_capturing) return is_actively_capturing < other.is_actively_capturing;
        return backreference_position < other.backreference_position;
    }
};

struct ActiveNFAState {
    std::shared_ptr<NFAState> nfa_state;
    CaptureGroupInfo capture_info;
    
    bool operator<(const ActiveNFAState& other) const {
        if (nfa_state != other.nfa_state) return nfa_state < other.nfa_state;
        return capture_info < other.capture_info;
    }
};

using ActiveStateList = std::vector<ActiveNFAState>;

bool has_matching_state(const ActiveStateList& active_states) {
    return std::any_of(active_states.begin(), active_states.end(), [](const ActiveNFAState& state) {
        return state.nfa_state->character_code == OPCODE_MATCHED;
    });
}

void add_state_with_epsilon_closure(std::shared_ptr<NFAState> state_to_add, 
                                   CaptureGroupInfo capture_info, 
                                   ActiveStateList& active_states, 
                                   std::set<std::shared_ptr<NFAState>>& visited_states) {
    if (!state_to_add || visited_states.count(state_to_add)) return;
    visited_states.insert(state_to_add);

    if (state_to_add->capture_group_start >= 0) {
        int group_id = state_to_add->capture_group_start;
        capture_info.captured_text[group_id].clear();
        capture_info.is_actively_capturing[group_id] = true;
    }
    if (state_to_add->capture_group_end >= 0) {
        int group_id = state_to_add->capture_group_end;
        capture_info.is_actively_capturing[group_id] = false;
    }

    if (state_to_add->character_code == OPCODE_SPLIT) {
        add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, active_states, visited_states);
        add_state_with_epsilon_closure(state_to_add->alternative_transition, capture_info, active_states, visited_states);
        return;
    }

    active_states.push_back({ state_to_add, capture_info });
}

void initialize_active_states(std::shared_ptr<NFAState> start_state, ActiveStateList& active_states) {
    active_states.clear();
    std::set<std::shared_ptr<NFAState>> visited_states;
    CaptureGroupInfo initial_capture_info{};
    add_state_with_epsilon_closure(start_state, initial_capture_info, active_states, visited_states);
}

void process_character_step(ActiveStateList& current_states, char input_char, ActiveStateList& next_states) {
    next_states.clear();

    for (const ActiveNFAState& active_state : current_states) {
        auto nfa_state = active_state.nfa_state;
        auto capture_info = active_state.capture_info;

        bool can_consume_character = false;
        bool character_added_by_backref = false;
        int backref_group_id = -1;

        switch (nfa_state->character_code) {
            case OPCODE_MATCH_ANY:
                can_consume_character = true;
                break;
            case OPCODE_MATCH_DIGIT:
                can_consume_character = isdigit(static_cast<unsigned char>(input_char));
                break;
            case OPCODE_MATCH_WORD:
                can_consume_character = (isalnum(static_cast<unsigned char>(input_char)) || input_char == '_');
                break;
            case OPCODE_MATCH_CHOICE:
                can_consume_character = (std::find(nfa_state->character_set.begin(), 
                                                  nfa_state->character_set.end(), 
                                                  input_char) != nfa_state->character_set.end());
                break;
            case OPCODE_MATCH_ANTI_CHOICE:
                can_consume_character = (std::find(nfa_state->character_set.begin(), 
                                                  nfa_state->character_set.end(), 
                                                  input_char) == nfa_state->character_set.end());
                break;
            default:
                if (nfa_state->character_code == input_char) {
                    can_consume_character = true;
                } else if (nfa_state->character_code >= OPCODE_BACKREF_START) {
                    backref_group_id = nfa_state->character_code - OPCODE_BACKREF_START;
                    auto captured_text_iter = capture_info.captured_text.find(backref_group_id);
                    if (captured_text_iter != capture_info.captured_text.end()) {
                        const std::string& previously_captured = captured_text_iter->second;
                        if (!previously_captured.empty()) {
                            size_t current_position = capture_info.backreference_position[backref_group_id];
                            if (current_position < previously_captured.size() && previously_captured[current_position] == input_char) {
                                current_position++;
                                for (auto& [group_id, is_active] : capture_info.is_actively_capturing) {
                                    if (is_active) capture_info.captured_text[group_id].push_back(input_char);
                                }
                                character_added_by_backref = true;
                                
                                if (current_position == previously_captured.size()) {
                                    capture_info.backreference_position[backref_group_id] = 0;
                                    can_consume_character = true;
                                } else {
                                    capture_info.backreference_position[backref_group_id] = current_position;
                                    next_states.push_back({ nfa_state, capture_info });
                                    continue;
                                }
                            }
                        }
                    }
                }
                break;
        }

        if (can_consume_character) {
            if (!character_added_by_backref) {
                for (auto& [group_id, is_active] : capture_info.is_actively_capturing) {
                    if (is_active) {
                        capture_info.captured_text[group_id].push_back(input_char);
                    }
                }
            }

            std::set<std::shared_ptr<NFAState>> visited_states;
            add_state_with_epsilon_closure(nfa_state->primary_transition, capture_info, next_states, visited_states);
        }
    }
}

// NEW: Structure to hold match position information
struct MatchInfo {
    bool found;
    size_t start_pos;
    size_t end_pos;
};

// NEW: Enhanced matching function that returns match positions
MatchInfo match_text_with_positions(std::shared_ptr<NFAState> nfa_start_state, std::string_view input_text) {
    ActiveStateList current_states, next_states;
    
    bool is_anchored_at_start = (nfa_start_state->character_code == OPCODE_MATCH_START);
    if (is_anchored_at_start) {
        std::set<std::shared_ptr<NFAState>> visited_states;
        CaptureGroupInfo initial_capture_info{};
        add_state_with_epsilon_closure(nfa_start_state->primary_transition, initial_capture_info, current_states, visited_states);
    } else {
        initialize_active_states(nfa_start_state, current_states);
    }

    const size_t text_length = input_text.size();
    size_t match_start = 0;
    
    for (size_t character_index = 0; character_index <= text_length; ++character_index) {
        if (character_index == text_length) {
            ActiveStateList end_of_string_states;
            for (const auto& active_state : current_states) {
                if (active_state.nfa_state->character_code == OPCODE_MATCH_END) {
                    std::set<std::shared_ptr<NFAState>> visited_states;
                    add_state_with_epsilon_closure(active_state.nfa_state->primary_transition, 
                                                  active_state.capture_info, 
                                                  end_of_string_states, 
                                                  visited_states);
                } else {
                    end_of_string_states.push_back(active_state);
                }
            }
            if (has_matching_state(end_of_string_states)) {
                return {true, match_start, character_index};
            }
        }

        if (character_index == text_length) break;

        char current_char = input_text[character_index];

        if (current_states.empty() && !is_anchored_at_start) {
            initialize_active_states(nfa_start_state, current_states);
            match_start = character_index;
        }

        if (!current_states.empty()) {
            process_character_step(current_states, current_char, next_states);
            current_states.swap(next_states);
        }

        if (has_matching_state(current_states)) {
            return {true, match_start, character_index + 1};
        }

        if (current_states.empty() && !is_anchored_at_start) {
            initialize_active_states(nfa_start_state, current_states);
            match_start = character_index;
            if (!current_states.empty()) {
                process_character_step(current_states, current_char, next_states);
                current_states.swap(next_states);
                if (has_matching_state(current_states)) {
                    return {true, match_start, character_index + 1};
                }
            }
        }
    }

    return {false, 0, 0};
}

// Original matching function (kept for compatibility)
int match_text_with_nfa(std::shared_ptr<NFAState> nfa_start_state, std::string_view input_text) {
    return match_text_with_positions(nfa_start_state, input_text).found ? 1 : 0;
}

// NEW: Function to print line with colorized match
void print_with_color(const std::string& line, const MatchInfo& match_info, bool use_color) {
    if (!use_color || !match_info.found) {
        std::cout << line << '\n';
        return;
    }
    
    // Print: before_match + [COLOR]match[RESET] + after_match
    std::cout << line.substr(0, match_info.start_pos)
              << COLOR_RED_BOLD
              << line.substr(match_info.start_pos, match_info.end_pos - match_info.start_pos)
              << COLOR_RESET
              << line.substr(match_info.end_pos)
              << '\n';
}

// --- Main Program ---
int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [-r] [--color=auto|always|never] -E pattern [file ...]\n";
        return 1;
    }

    bool found_extended_regex_flag = false;
    bool use_recursive_search = false;
    bool use_color = true;  // Default: auto (enabled)
    std::string regex_pattern_string;
    std::vector<std::string> target_file_paths;

    for (int arg_index = 1; arg_index < argc; arg_index++) {
        std::string current_argument = argv[arg_index];

        if (current_argument == "-E") {
            found_extended_regex_flag = true;
            if (++arg_index < argc) {
                regex_pattern_string = argv[arg_index];
            } else {
                std::cerr << "Error: -E requires a pattern argument\n";
                return 1;
            }
        } else if (current_argument == "-r") {
            use_recursive_search = true;
        } else if (current_argument.find("--color=") == 0) {
            std::string color_option = current_argument.substr(8);
            if (color_option == "always") {
                use_color = true;
            } else if (color_option == "never") {
                use_color = false;
            } else if (color_option == "auto") {
                use_color = true;  // On Windows, we default to enabled
            }
        } else {
            target_file_paths.push_back(current_argument);
        }
    }

    if (!found_extended_regex_flag) {
        std::cerr << "Error: Expected -E followed by a pattern.\n";
        return 1;
    }
    if (regex_pattern_string.empty()) {
        std::cerr << "Error: Pattern cannot be empty.\n";
        return 1;
    }

    std::shared_ptr<NFAState> compiled_nfa_start;
    try {
        compiled_nfa_start = compile_regex_to_nfa(regex_pattern_string);
    } catch (const std::runtime_error& parsing_error) {
        std::cerr << "Regex parsing error: " << parsing_error.what() << '\n';
        return 1;
    }

    bool found_any_match_globally = false;

    if (target_file_paths.empty()) {
        if (use_recursive_search) {
            target_file_paths.push_back(".");
        } else {
            std::string input_line;
            while (std::getline(std::cin, input_line)) {
                MatchInfo match_info = match_text_with_positions(compiled_nfa_start, input_line);
                if (match_info.found) {
                    print_with_color(input_line, match_info, use_color);
                    found_any_match_globally = true;
                }
            }
            return found_any_match_globally ? 0 : 1;
        }
    }

    std::vector<std::string> files_to_search;
    if (use_recursive_search) {
        for (const auto& target_path : target_file_paths) {
            auto discovered_files = find_all_files_recursively(target_path);
            files_to_search.insert(files_to_search.end(), discovered_files.begin(), discovered_files.end());
        }
    } else {
        for (const auto& target_path : target_file_paths) {
            if (fs::is_regular_file(target_path)) {
                files_to_search.push_back(target_path);
            } else if (fs::exists(target_path)) {
                std::cerr << "Warning: Skipping non-regular file: " << target_path << '\n';
            } else {
                std::cerr << "Error: Path not found: " << target_path << '\n';
            }
        }
    }

    bool should_prefix_with_filename = (files_to_search.size() > 1);

    for (const auto& file_path : files_to_search) {
        std::ifstream input_file_stream(file_path);
        if (!input_file_stream.is_open()) {
            std::cerr << "Error: Could not open file " << file_path << '\n';
            continue;
        }

        std::string file_line;
        while (std::getline(input_file_stream, file_line)) {
            MatchInfo match_info = match_text_with_positions(compiled_nfa_start, file_line);
            if (match_info.found) {
                if (should_prefix_with_filename) {
                    std::cout << file_path << ':';
                }
                print_with_color(file_line, match_info, use_color);
                found_any_match_globally = true;
            }
        }
        input_file_stream.close();
    }

    return !found_any_match_globally;
}