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
/**
 * NFAState represents a single state in the NFA (Non-deterministic Finite Automaton).
 * 
 * An NFA is a theoretical machine used to recognize patterns. Unlike DFA (Deterministic FA),
 * an NFA can be in multiple states simultaneously and can have epsilon transitions
 * (transitions without consuming input).
 */
struct NFAState {
    // Pointers to next states - NFA can transition to multiple states
    std::shared_ptr<NFAState> primary_transition, alternative_transition;  // primary is main path, alternative is for splits
    
    int character_code = -1;  // Character this state matches, or special opcode (see enum below)
    
    // Used to prevent infinite loops during NFA simulation
    // Each simulation step gets a unique ID to track visited states
    int last_list_id = -1;
    
    // For character class matching like [abc] - stores the set of valid characters
    std::vector<int> character_set;

    // Capture group support - allows extracting matched substrings
    int capture_group_start = -1;  // If >= 0, this state starts capture group N
    int capture_group_end = -1;    // If >= 0, this state ends capture group N
};

// --- NFA Fragment Definition ---
/**
 * NFAFragment represents a partially constructed NFA during parsing.
 * 
 * When parsing regex patterns, we build the NFA incrementally. A fragment
 * has a starting state and a list of "dangling" output pointers that need
 * to be connected to the next part of the pattern.
 */
struct NFAFragment {
    std::shared_ptr<NFAState> start_state;  // Entry point of this fragment
    
    // List of pointers to NFAState* that need to be filled in
    // These represent incomplete transitions that will be connected later
    std::vector<std::shared_ptr<NFAState>*> dangling_outputs;
};

// --- NFA Opcodes ---
/**
 * Special opcodes for NFA states. Regular characters use their ASCII values,
 * but special regex constructs need unique identifiers > 255.
 */
enum RegexOpcodes {
    OPCODE_SPLIT = 256,        // Epsilon split - go to both transitions without consuming input
    OPCODE_MATCH_ANY,          // '.' - matches any character except newline
    OPCODE_MATCH_WORD,         // '\w' - matches word characters [a-zA-Z0-9_]
    OPCODE_MATCH_DIGIT,        // '\d' - matches digits [0-9]
    OPCODE_MATCH_CHOICE,       // '[abc]' - matches any character in the set
    OPCODE_MATCH_ANTI_CHOICE,  // '[^abc]' - matches any character NOT in the set
    OPCODE_MATCH_START,        // '^' - matches start of string/line
    OPCODE_MATCH_END,          // '$' - matches end of string/line
    OPCODE_BACKREF_START = 300, // '\1', '\2' etc. - backreferences to capture groups
    OPCODE_MATCHED = 1000      // Special state indicating successful match
};

// --- Global Counters ---
// Used to assign unique IDs to capture groups during parsing
int next_capture_group_id = 1;

// --- Utility Functions ---
/**
 * Recursively finds all files in a directory tree.
 * Used when the -r (recursive) flag is specified.
 */
std::vector<std::string> find_all_files_recursively(fs::path directory_path) {
    std::vector<std::string> found_files;

    // If it's not a directory, check if it's a regular file
    if (!fs::is_directory(directory_path)) {
        if (fs::is_regular_file(directory_path)) {
            return {directory_path.string()};
        }
        return {};
    }

    // Recursively iterate through all files in the directory
    for (auto& entry : fs::recursive_directory_iterator(directory_path)) {
        if (!fs::is_directory(entry)) {
            found_files.push_back(entry.path().string());
        }
    }
    return found_files;
}

// Forward declaration needed because parse_regex() and parse_primary_element() call each other
struct NFAFragment;
NFAFragment parse_regex(std::string_view&, NFAFragment, int);

// --- NFA Construction ---
/**
 * parse_primary_element() handles the basic building blocks of regex patterns.
 * This includes:
 * - Literal characters (a, b, c, etc.)
 * - Special characters (., ^, $)
 * - Escape sequences (\d, \w, \1, etc.)
 * - Character classes ([abc], [^abc])
 * - Groups ((pattern))
 * - Quantifiers (*, +, ?)
 */
NFAFragment parse_primary_element(std::string_view& regex_pattern) {
    if (regex_pattern.empty()) {
        throw std::runtime_error("Unexpected end of pattern");
    }

    // Create a new state for this pattern element
    auto new_state = std::make_shared<NFAState>();
    NFAFragment current_fragment { new_state, { &new_state->primary_transition } };

    // Get the current character and advance the pattern
    char current_char = regex_pattern.front();
    regex_pattern.remove_prefix(1);

    switch (current_char) {
        case '.':
            // '.' matches any character
            new_state->character_code = OPCODE_MATCH_ANY;
            break;
        case '^':
            // '^' matches start of string
            new_state->character_code = OPCODE_MATCH_START;
            break;
        case '$':
            // '$' matches end of string
            new_state->character_code = OPCODE_MATCH_END;
            break;
        case '\\': {
            // Escape sequence - next character has special meaning
            if (regex_pattern.empty()) throw std::runtime_error("Unexpected end of pattern after \\");
            current_char = regex_pattern.front();
            regex_pattern.remove_prefix(1);
            if (current_char == 'd') new_state->character_code = OPCODE_MATCH_DIGIT;       // \d = digits
            else if (current_char == 'w') new_state->character_code = OPCODE_MATCH_WORD;   // \w = word chars
            else if (isdigit(static_cast<unsigned char>(current_char))) {
                // \1, \2, etc. = backreferences to capture groups
                new_state->character_code = OPCODE_BACKREF_START + (current_char - '0');
            } else new_state->character_code = current_char;  // Escaped literal character
            break;
        }
        case '[': {
            // Character class [abc] or negated class [^abc]
            bool is_negated = false;
            if (!regex_pattern.empty() && regex_pattern.front() == '^') {
                is_negated = true;  // Negated character class
                regex_pattern.remove_prefix(1);
            }
            new_state->character_code = is_negated ? OPCODE_MATCH_ANTI_CHOICE : OPCODE_MATCH_CHOICE;

            // Collect all characters in the class
            while (!regex_pattern.empty() && regex_pattern.front() != ']') {
                new_state->character_set.push_back(regex_pattern.front());
                regex_pattern.remove_prefix(1);
            }
            if (regex_pattern.empty()) throw std::runtime_error("Unclosed bracket expression");
            regex_pattern.remove_prefix(1);  // Skip the closing ']'
            break;
        }
        case '(': {
            // Capture group (pattern) - creates a numbered group for backreferences
            int current_capture_group_id = next_capture_group_id++;

            // Create start marker state
            auto capture_start_state = std::make_shared<NFAState>();
            capture_start_state->character_code = OPCODE_SPLIT;
            capture_start_state->capture_group_start = current_capture_group_id;

            // Recursively parse the contents of the group
            NFAFragment inner_fragment = parse_regex(regex_pattern, parse_primary_element(regex_pattern), 0);

            // Expect closing parenthesis
            if (regex_pattern.empty() || regex_pattern.front() != ')') {
                throw std::runtime_error("Expected ')' to close group");
            }
            regex_pattern.remove_prefix(1);

            // Create end marker state
            auto capture_end_state = std::make_shared<NFAState>();
            capture_end_state->character_code = OPCODE_SPLIT;
            capture_end_state->capture_group_end = current_capture_group_id;

            // Connect the states: start -> inner_pattern -> end
            capture_start_state->primary_transition = inner_fragment.start_state;
            for (auto dangling_output : inner_fragment.dangling_outputs) {
                *dangling_output = capture_end_state;
            }

            current_fragment = { capture_start_state, { &capture_end_state->primary_transition } };
            break;
        }
        default:
            // Regular literal character
            new_state->character_code = current_char;
    }

    // Handle quantifiers (*, +, ?) after the basic pattern
    if (!regex_pattern.empty()) {
        switch (regex_pattern.front()) {
            case '*': {
                // Zero or more repetitions
                // Creates: Split -> (pattern -> Split) | epsilon
                auto split_state = std::make_shared<NFAState>();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;  // Go to pattern

                // Pattern loops back to the split state
                for (auto dangling_output : current_fragment.dangling_outputs) {
                    *dangling_output = split_state;
                }

                // New output is the epsilon transition
                current_fragment = { split_state, { &split_state->alternative_transition } };
                regex_pattern.remove_prefix(1);
            } break;
            case '+': {
                // One or more repetitions
                // Pattern must match at least once, then can repeat
                auto split_state = std::make_shared<NFAState>();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;  // Loop back to pattern

                // Connect pattern output to the split
                for (auto dangling_output : current_fragment.dangling_outputs) {
                    *dangling_output = split_state;
                }

                // Output is the epsilon transition from split
                current_fragment.dangling_outputs = { &split_state->alternative_transition };
                regex_pattern.remove_prefix(1);
            } break;
            case '?': {
                // Zero or one occurrence (optional)
                // Creates: Split -> pattern | epsilon
                auto split_state = std::make_shared<NFAState>();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;  // Go to pattern

                current_fragment.start_state = split_state;
                // Add epsilon transition as additional output
                current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
                regex_pattern.remove_prefix(1);
            } break;
        }
    }

    return current_fragment;
}

/**
 * parse_regex() handles operator precedence and builds the complete NFA.
 * It implements a precedence climbing parser for regex operators.
 * 
 * Operator precedence (highest to lowest):
 * 1. Quantifiers (*, +, ?) - handled in parse_primary_element
 * 2. Concatenation (implicit) - precedence 1
 * 3. Alternation (|) - precedence 0
 */
NFAFragment parse_regex(std::string_view& regex_pattern, NFAFragment left_fragment, int min_precedence) {
    // Define operator precedence
    auto get_precedence = [](char operator_char) {
        if (operator_char == '|') return 0;        // Lowest precedence - alternation
        if (operator_char == ']' || operator_char == ')') return -1;  // End markers
        return 1;                      // Concatenation (implicit)
    };

    char lookahead_char = !regex_pattern.empty() ? regex_pattern.front() : '\0';

    // Process operators while precedence is high enough
    while (!regex_pattern.empty() && get_precedence(lookahead_char) >= min_precedence) {
        char current_operator = lookahead_char;

        if (current_operator == '|') {
            regex_pattern.remove_prefix(1);  // Consume the '|'
        }

        // Parse the right-hand side
        NFAFragment right_fragment = parse_primary_element(regex_pattern);

        // Look ahead for the next operator
        if (!regex_pattern.empty()) {
            lookahead_char = regex_pattern.front();
        } else {
            lookahead_char = '\0';
        }

        // Handle right-associative operators with higher precedence
        while (!regex_pattern.empty() && get_precedence(lookahead_char) > get_precedence(current_operator)) {
            right_fragment = parse_regex(regex_pattern, right_fragment, get_precedence(current_operator) + 1);
            if (regex_pattern.empty()) break;
            lookahead_char = regex_pattern.front();
        }

        // Combine left and right fragments based on operator
        if (current_operator == '|') {
            // Alternation: Split -> left_fragment | right_fragment
            auto split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = left_fragment.start_state;   // First alternative
            split_state->alternative_transition = right_fragment.start_state;  // Second alternative

            left_fragment.start_state = split_state;
            // Combine outputs from both alternatives
            left_fragment.dangling_outputs.insert(left_fragment.dangling_outputs.end(), 
                                                  right_fragment.dangling_outputs.begin(), 
                                                  right_fragment.dangling_outputs.end());
        } else {
            // Concatenation: left_fragment -> right_fragment
            // Connect all outputs of left_fragment to start of right_fragment
            for (auto dangling_output : left_fragment.dangling_outputs) {
                *dangling_output = right_fragment.start_state;
            }
            left_fragment.dangling_outputs = right_fragment.dangling_outputs;  // New outputs are right_fragment outputs
        }

        if (regex_pattern.empty()) break;
        lookahead_char = regex_pattern.front();
    }

    return left_fragment;
}

/**
 * compile_regex_to_nfa() is the main entry point for converting a regex string to an NFA.
 * It parses the entire pattern and creates a complete NFA with a final "Matched" state.
 */
std::shared_ptr<NFAState> compile_regex_to_nfa(std::string_view regex_string) {
    // Create the final accepting state
    auto matched_state = std::make_shared<NFAState>();
    matched_state->character_code = OPCODE_MATCHED;

    if (regex_string.empty()) return matched_state;

    std::string_view remaining_pattern = regex_string;
    next_capture_group_id = 1;  // Reset capture group counter

    // Parse the entire regex pattern
    NFAFragment complete_fragment = parse_regex(remaining_pattern, parse_primary_element(remaining_pattern), 0);

    // Check for syntax errors
    if (!remaining_pattern.empty()) {
        if (remaining_pattern.front() == ')') throw std::runtime_error("Unmatched ')'");
        if (remaining_pattern.front() == ']') throw std::runtime_error("Unmatched ']'");
        throw std::runtime_error("Syntax error in regex");
    }

    // Connect all dangling outputs to the final matched state
    for (auto dangling_output : complete_fragment.dangling_outputs) {
        *dangling_output = matched_state;
    }

    return complete_fragment.start_state;
}

// --- NFA Simulation with Captures ---

/**
 * CaptureGroupInfo tracks the state of capture groups during NFA simulation.
 * As we simulate the NFA, we need to track what text each capture group has matched.
 */
struct CaptureGroupInfo {
    std::map<int, std::string> captured_text;      // Captured text for each group ID
    std::map<int, bool> is_actively_capturing;     // Which groups are currently capturing
    std::map<int, size_t> backreference_position;  // Position for backreference matching
    
    // Needed for using CaptureGroupInfo in std::set (for deduplication)
    bool operator<(const CaptureGroupInfo& other) const {
        if (captured_text != other.captured_text) return captured_text < other.captured_text;
        if (is_actively_capturing != other.is_actively_capturing) return is_actively_capturing < other.is_actively_capturing;
        return backreference_position < other.backreference_position;
    }
};

/**
 * ActiveNFAState represents a single state in the NFA simulation.
 * The NFA can be in multiple states simultaneously, each with its own capture info.
 */
struct ActiveNFAState {
    std::shared_ptr<NFAState> nfa_state;  // The NFA state
    CaptureGroupInfo capture_info;        // Capture group information for this path
    
    // Needed for deduplication in std::set
    bool operator<(const ActiveNFAState& other) const {
        if (nfa_state != other.nfa_state) return nfa_state < other.nfa_state;
        return capture_info < other.capture_info;
    }
};

// List of active NFA states
using ActiveStateList = std::vector<ActiveNFAState>;

/**
 * Check if any state in the current list is a "Matched" state.
 * If so, the pattern has successfully matched the input.
 */
bool has_matching_state(const ActiveStateList& active_states) {
    return std::any_of(active_states.begin(), active_states.end(), [](const ActiveNFAState& state) {
        return state.nfa_state->character_code == OPCODE_MATCHED;
    });
}

/**
 * add_state_with_epsilon_closure() adds a state to the current list, handling epsilon transitions.
 * 
 * Epsilon transitions (like Split states) don't consume input characters.
 * We need to follow all epsilon transitions before processing the next input character.
 */
void add_state_with_epsilon_closure(std::shared_ptr<NFAState> state_to_add, 
                                   CaptureGroupInfo capture_info, 
                                   ActiveStateList& active_states, 
                                   std::set<std::shared_ptr<NFAState>>& visited_states) {
    if (!state_to_add || visited_states.count(state_to_add)) return;  // Avoid infinite loops
    visited_states.insert(state_to_add);

    // Handle capture group markers
    if (state_to_add->capture_group_start >= 0) {
        int group_id = state_to_add->capture_group_start;
        capture_info.captured_text[group_id].clear();     // Start fresh capture
        capture_info.is_actively_capturing[group_id] = true;      // Begin capturing
    }
    if (state_to_add->capture_group_end >= 0) {
        int group_id = state_to_add->capture_group_end;
        capture_info.is_actively_capturing[group_id] = false;     // Stop capturing
    }

    // Handle epsilon transitions (Split states)
    if (state_to_add->character_code == OPCODE_SPLIT) {
        // Follow both paths without consuming input
        add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, active_states, visited_states);
        add_state_with_epsilon_closure(state_to_add->alternative_transition, capture_info, active_states, visited_states);
        return;
    }

    // This is a regular state - add it to the active list
    active_states.push_back({ state_to_add, capture_info });
}

/**
 * Initialize the NFA simulation by adding the start state and all epsilon-reachable states.
 */
void initialize_active_states(std::shared_ptr<NFAState> start_state, ActiveStateList& active_states) {
    active_states.clear();
    std::set<std::shared_ptr<NFAState>> visited_states;
    CaptureGroupInfo initial_capture_info{};  // Empty initial capture info
    add_state_with_epsilon_closure(start_state, initial_capture_info, active_states, visited_states);
}

/**
 * process_character_step() simulates one step of NFA execution.
 * For each active state, check if it can consume the current input character.
 * If so, transition to the next state and add it to the next list.
 */
void process_character_step(ActiveStateList& current_states, char input_char, ActiveStateList& next_states) {
    next_states.clear();  // Clear the "next list"

    // Process each currently active NFA state
    for (const ActiveNFAState& active_state : current_states) {
        auto nfa_state = active_state.nfa_state;
        auto capture_info = active_state.capture_info;

        bool can_consume_character = false;
        bool character_added_by_backref = false;
        int backref_group_id = -1;

        // Check if this state can consume the current character
        switch (nfa_state->character_code) {
            case OPCODE_MATCH_ANY:
                // '.' matches any character
                can_consume_character = true;
                break;
            case OPCODE_MATCH_DIGIT:
                // '\d' matches digits
                can_consume_character = isdigit(static_cast<unsigned char>(input_char));
                break;
            case OPCODE_MATCH_WORD:
                // '\w' matches word characters
                can_consume_character = (isalnum(static_cast<unsigned char>(input_char)) || input_char == '_');
                break;
            case OPCODE_MATCH_CHOICE:
                // '[abc]' matches characters in the set
                can_consume_character = (std::find(nfa_state->character_set.begin(), 
                                                  nfa_state->character_set.end(), 
                                                  input_char) != nfa_state->character_set.end());
                break;
            case OPCODE_MATCH_ANTI_CHOICE:
                // '[^abc]' matches characters NOT in the set
                can_consume_character = (std::find(nfa_state->character_set.begin(), 
                                                  nfa_state->character_set.end(), 
                                                  input_char) == nfa_state->character_set.end());
                break;
            default:
                if (nfa_state->character_code == input_char) {
                    // Literal character match
                    can_consume_character = true;
                } else if (nfa_state->character_code >= OPCODE_BACKREF_START) {
                    // Backreference matching (\1, \2, etc.)
                    backref_group_id = nfa_state->character_code - OPCODE_BACKREF_START;
                    auto captured_text_iter = capture_info.captured_text.find(backref_group_id);
                    if (captured_text_iter != capture_info.captured_text.end()) {
                        const std::string& previously_captured = captured_text_iter->second;
                        if (!previously_captured.empty()) {
                            // Check if current character matches the backreference
                            size_t current_position = capture_info.backreference_position[backref_group_id];
                            if (current_position < previously_captured.size() && previously_captured[current_position] == input_char) {
                                current_position++;
                                // Update active captures
                                for (auto& [group_id, is_active] : capture_info.is_actively_capturing) {
                                    if (is_active) capture_info.captured_text[group_id].push_back(input_char);
                                }
                                character_added_by_backref = true;
                                
                                if (current_position == previously_captured.size()) {
                                    // Finished matching the backreference
                                    capture_info.backreference_position[backref_group_id] = 0;
                                    can_consume_character = true;
                                } else {
                                    // Still matching the backreference
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

        // If we can advance, update captures and transition to next state
        if (can_consume_character) {
            if (!character_added_by_backref) {
                // Update all active capture groups with the current character
                for (auto& [group_id, is_active] : capture_info.is_actively_capturing) {
                    if (is_active) {
                        capture_info.captured_text[group_id].push_back(input_char);
                    }
                }
            }

            // Add the next state (following epsilon transitions)
            std::set<std::shared_ptr<NFAState>> visited_states;
            add_state_with_epsilon_closure(nfa_state->primary_transition, capture_info, next_states, visited_states);
        }
    }
}

/**
 * match_text_with_nfa() is the main matching function.
 * It simulates the NFA on the given input text and returns 1 if match found, 0 otherwise.
 */
int match_text_with_nfa(std::shared_ptr<NFAState> nfa_start_state, std::string_view input_text) {
    ActiveStateList current_states, next_states;  // Current list and next list of active states

    // Handle anchored patterns (starting with ^)
    bool is_anchored_at_start = (nfa_start_state->character_code == OPCODE_MATCH_START);
    if (is_anchored_at_start) {
        // Skip the MatchStart state and start from its output
        std::set<std::shared_ptr<NFAState>> visited_states;
        CaptureGroupInfo initial_capture_info{};
        add_state_with_epsilon_closure(nfa_start_state->primary_transition, initial_capture_info, current_states, visited_states);
    } else {
        // Normal pattern - start with the initial state
        initialize_active_states(nfa_start_state, current_states);
    }

    const size_t text_length = input_text.size();
    
    // Process each character in the input
    for (size_t character_index = 0; character_index <= text_length; ++character_index) {
        // Handle end-of-string anchors ($)
        if (character_index == text_length) {
            ActiveStateList end_of_string_states;
            for (const auto& active_state : current_states) {
                if (active_state.nfa_state->character_code == OPCODE_MATCH_END) {
                    // This state matches end-of-string
                    std::set<std::shared_ptr<NFAState>> visited_states;
                    add_state_with_epsilon_closure(active_state.nfa_state->primary_transition, 
                                                  active_state.capture_info, 
                                                  end_of_string_states, 
                                                  visited_states);
                } else {
                    end_of_string_states.push_back(active_state);
                }
            }
            if (has_matching_state(end_of_string_states)) return 1;
        }

        if (character_index == text_length) break;  // Processed all characters

        char current_char = input_text[character_index];

        // For unanchored patterns, try to start matching at each position
        if (current_states.empty() && !is_anchored_at_start) {
            initialize_active_states(nfa_start_state, current_states);
        }

        // Process current character with all active states
        if (!current_states.empty()) {
            process_character_step(current_states, current_char, next_states);
            current_states.swap(next_states);  // Next list becomes current list
        }

        // Check if we have a match after this character
        if (has_matching_state(current_states)) {
            return 1;
        }

        // For unanchored patterns, also try starting a new match at this position
        if (current_states.empty() && !is_anchored_at_start) {
            initialize_active_states(nfa_start_state, current_states);
            if (!current_states.empty()) {
                process_character_step(current_states, current_char, next_states);
                current_states.swap(next_states);
                if (has_matching_state(current_states)) return 1;
            }
        }
    }

    return 0;  // No match found
}

// --- Main Program ---
/**
 * Main function - implements a grep-like utility using our regex engine.
 * 
 * Usage: program [-r] -E pattern [file ...]
 * -r: recursive directory search
 * -E: extended regex pattern (required)
 * 
 * If no files specified, reads from stdin.
 * Returns 0 if matches found, 1 otherwise.
 */
int main(int argc, char* argv[]) {
    // Enable immediate output flushing
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [-r] -E pattern [file ...]\n";
        return 1;
    }

    // Parse command line arguments
    bool found_extended_regex_flag = false;
    bool use_recursive_search = false;
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
        } else {
            target_file_paths.push_back(current_argument);
        }
    }

    // Validate arguments
    if (!found_extended_regex_flag) {
        std::cerr << "Error: Expected -E followed by a pattern.\n";
        return 1;
    }
    if (regex_pattern_string.empty()) {
        std::cerr << "Error: Pattern cannot be empty.\n";
        return 1;
    }

    // Compile the regex pattern to NFA
    std::shared_ptr<NFAState> compiled_nfa_start;
    try {
        compiled_nfa_start = compile_regex_to_nfa(regex_pattern_string);
    } catch (const std::runtime_error& parsing_error) {
        std::cerr << "Regex parsing error: " << parsing_error.what() << '\n';
        return 1;
    }

    bool found_any_match_globally = false;

    // Handle different input modes
    if (target_file_paths.empty()) {
        if (use_recursive_search) {
            // No paths given but -r specified: search current directory
            target_file_paths.push_back(".");
        } else {
            // No paths given: read from stdin
            std::string input_line;
            while (std::getline(std::cin, input_line)) {
                if (match_text_with_nfa(compiled_nfa_start, input_line)) {
                    std::cout << input_line << '\n';
                    found_any_match_globally = true;
                }
            }
            return found_any_match_globally ? 0 : 1;
        }
    }

    // Build list of files to process
    std::vector<std::string> files_to_search;
    if (use_recursive_search) {
        // Recursively find all files in specified directories
        for (const auto& target_path : target_file_paths) {
            auto discovered_files = find_all_files_recursively(target_path);
            files_to_search.insert(files_to_search.end(), discovered_files.begin(), discovered_files.end());
        }
    } else {
        // Process specified files directly
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

    // Determine output format (prefix with filename if multiple files)
    bool should_prefix_with_filename = (files_to_search.size() > 1);

    // Process each file
    for (const auto& file_path : files_to_search) {
        std::ifstream input_file_stream(file_path);
        if (!input_file_stream.is_open()) {
            std::cerr << "Error: Could not open file " << file_path << '\n';
            continue;
        }

        // Check each line in the file
        std::string file_line;
        while (std::getline(input_file_stream, file_line)) {
            if (match_text_with_nfa(compiled_nfa_start, file_line)) {
                if (should_prefix_with_filename) {
                    std::cout << file_path << ':';
                }
                std::cout << file_line << '\n';
                found_any_match_globally = true;
            }
        }
        input_file_stream.close();
    }

    // Return 0 if any matches found, 1 otherwise (grep-like behavior)
    return !found_any_match_globally;
}