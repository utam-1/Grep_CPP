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

// --- Global Profiling Counters ---
struct NFAProfiler
{
    size_t total_steps = 0;
    size_t total_states_visited = 0;
    size_t max_active_states = 0;
    size_t lines_processed = 0;

    void reset()
    {
        total_steps = 0;
        total_states_visited = 0;
        max_active_states = 0;
        lines_processed = 0;
    }
};

NFAProfiler profiler;
bool enable_profiling = false;

// --- NFA State Definition ---
struct NFAState
{
    std::shared_ptr<NFAState> primary_transition, alternative_transition;
    int character_code = -1;
    int last_list_id = -1;
    std::vector<int> character_set;
    int capture_group_start = -1;
    int capture_group_end = -1;
};

// --- NFA Fragment Definition ---
struct NFAFragment
{
    std::shared_ptr<NFAState> start_state;
    std::vector<std::shared_ptr<NFAState> *> dangling_outputs;
};

// --- NFA Opcodes ---
enum RegexOpcodes
{
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
std::vector<std::string> find_all_files_recursively(fs::path directory_path)
{
    std::vector<std::string> found_files;

    if (!fs::is_directory(directory_path))
    {
        if (fs::is_regular_file(directory_path))
        {
            return {directory_path.string()};
        }
        return {};
    }

    for (auto &entry : fs::recursive_directory_iterator(directory_path))
    {
        if (!fs::is_directory(entry))
        {
            found_files.push_back(entry.path().string());
        }
    }
    return found_files;
}

// Forward declaration
struct NFAFragment;
NFAFragment parse_regex(std::string_view &, NFAFragment, int);

// --- NFA Construction ---
NFAFragment parse_primary_element(std::string_view &regex_pattern)
{
    if (regex_pattern.empty())
    {
        throw std::runtime_error("Unexpected end of pattern");
    }

    auto new_state = std::make_shared<NFAState>();
    NFAFragment current_fragment{new_state, {&new_state->primary_transition}};

    char current_char = regex_pattern.front();
    regex_pattern.remove_prefix(1);

    switch (current_char)
    {
    case '.':
        new_state->character_code = OPCODE_MATCH_ANY;
        break;
    case '^':
        new_state->character_code = OPCODE_MATCH_START;
        break;
    case '$':
        new_state->character_code = OPCODE_MATCH_END;
        break;
    case '\\':
    {
        if (regex_pattern.empty())
            throw std::runtime_error("Unexpected end of pattern after \\");
        current_char = regex_pattern.front();
        regex_pattern.remove_prefix(1);
        if (current_char == 'd')
            new_state->character_code = OPCODE_MATCH_DIGIT;
        else if (current_char == 'w')
            new_state->character_code = OPCODE_MATCH_WORD;
        else if (isdigit(static_cast<unsigned char>(current_char)))
        {
            new_state->character_code = OPCODE_BACKREF_START + (current_char - '0');
        }
        else
            new_state->character_code = current_char;
        break;
    }
    case '[':
    {
        bool is_negated = false;
        if (!regex_pattern.empty() && regex_pattern.front() == '^')
        {
            is_negated = true;
            regex_pattern.remove_prefix(1);
        }
        new_state->character_code = is_negated ? OPCODE_MATCH_ANTI_CHOICE : OPCODE_MATCH_CHOICE;

        while (!regex_pattern.empty() && regex_pattern.front() != ']')
        {
            new_state->character_set.push_back(regex_pattern.front());
            regex_pattern.remove_prefix(1);
        }
        if (regex_pattern.empty())
            throw std::runtime_error("Unclosed bracket expression");
        regex_pattern.remove_prefix(1);
        break;
    }
    case '(':
    {
        int current_capture_group_id = next_capture_group_id++;

        auto capture_start_state = std::make_shared<NFAState>();
        capture_start_state->character_code = OPCODE_SPLIT;
        capture_start_state->capture_group_start = current_capture_group_id;

        NFAFragment inner_fragment = parse_regex(regex_pattern, parse_primary_element(regex_pattern), 0);

        if (regex_pattern.empty() || regex_pattern.front() != ')')
        {
            throw std::runtime_error("Expected ')' to close group");
        }
        regex_pattern.remove_prefix(1);

        auto capture_end_state = std::make_shared<NFAState>();
        capture_end_state->character_code = OPCODE_SPLIT;
        capture_end_state->capture_group_end = current_capture_group_id;

        capture_start_state->primary_transition = inner_fragment.start_state;
        for (auto dangling_output : inner_fragment.dangling_outputs)
        {
            *dangling_output = capture_end_state;
        }

        current_fragment = {capture_start_state, {&capture_end_state->primary_transition}};
        break;
    }
    default:
        new_state->character_code = current_char;
    }

    if (!regex_pattern.empty())
    {
        switch (regex_pattern.front())
        {
        case '*':
        {
            auto split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = current_fragment.start_state;

            for (auto dangling_output : current_fragment.dangling_outputs)
            {
                *dangling_output = split_state;
            }

            current_fragment = {split_state, {&split_state->alternative_transition}};
            regex_pattern.remove_prefix(1);
        }
        break;
        case '+':
        {
            auto split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = current_fragment.start_state;

            for (auto dangling_output : current_fragment.dangling_outputs)
            {
                *dangling_output = split_state;
            }

            current_fragment.dangling_outputs = {&split_state->alternative_transition};
            regex_pattern.remove_prefix(1);
        }
        break;
        case '?':
        {
            auto split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = current_fragment.start_state;

            current_fragment.start_state = split_state;
            current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
            regex_pattern.remove_prefix(1);
        }
        break;
        }
    }

    return current_fragment;
}

NFAFragment parse_regex(std::string_view &regex_pattern, NFAFragment left_fragment, int min_precedence)
{
    auto get_precedence = [](char operator_char)
    {
        if (operator_char == '|')
            return 0;
        if (operator_char == ']' || operator_char == ')')
            return -1;
        return 1;
    };

    char lookahead_char = !regex_pattern.empty() ? regex_pattern.front() : '\0';

    while (!regex_pattern.empty() && get_precedence(lookahead_char) >= min_precedence)
    {
        char current_operator = lookahead_char;

        if (current_operator == '|')
        {
            regex_pattern.remove_prefix(1);
        }

        NFAFragment right_fragment = parse_primary_element(regex_pattern);

        if (!regex_pattern.empty())
        {
            lookahead_char = regex_pattern.front();
        }
        else
        {
            lookahead_char = '\0';
        }

        while (!regex_pattern.empty() && get_precedence(lookahead_char) > get_precedence(current_operator))
        {
            right_fragment = parse_regex(regex_pattern, right_fragment, get_precedence(current_operator) + 1);
            if (regex_pattern.empty())
                break;
            lookahead_char = regex_pattern.front();
        }

        if (current_operator == '|')
        {
            auto split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = left_fragment.start_state;
            split_state->alternative_transition = right_fragment.start_state;

            left_fragment.start_state = split_state;
            left_fragment.dangling_outputs.insert(left_fragment.dangling_outputs.end(),
                                                  right_fragment.dangling_outputs.begin(),
                                                  right_fragment.dangling_outputs.end());
        }
        else
        {
            for (auto dangling_output : left_fragment.dangling_outputs)
            {
                *dangling_output = right_fragment.start_state;
            }
            left_fragment.dangling_outputs = right_fragment.dangling_outputs;
        }

        if (regex_pattern.empty())
            break;
        lookahead_char = regex_pattern.front();
    }

    return left_fragment;
}

std::shared_ptr<NFAState> compile_regex_to_nfa(std::string_view regex_string)
{
    auto matched_state = std::make_shared<NFAState>();
    matched_state->character_code = OPCODE_MATCHED;

    if (regex_string.empty())
        return matched_state;

    std::string_view remaining_pattern = regex_string;
    next_capture_group_id = 1;

    NFAFragment complete_fragment = parse_regex(remaining_pattern, parse_primary_element(remaining_pattern), 0);

    if (!remaining_pattern.empty())
    {
        if (remaining_pattern.front() == ')')
            throw std::runtime_error("Unmatched ')'");
        if (remaining_pattern.front() == ']')
            throw std::runtime_error("Unmatched ']'");
        throw std::runtime_error("Syntax error in regex");
    }

    for (auto dangling_output : complete_fragment.dangling_outputs)
    {
        *dangling_output = matched_state;
    }

    return complete_fragment.start_state;
}

// --- NFA Simulation with Captures ---
struct CaptureGroupInfo
{
    std::map<int, std::string> captured_text;
    std::map<int, bool> is_actively_capturing;
    std::map<int, size_t> backreference_position;

    bool operator<(const CaptureGroupInfo &other) const
    {
        if (captured_text != other.captured_text)
            return captured_text < other.captured_text;
        if (is_actively_capturing != other.is_actively_capturing)
            return is_actively_capturing < other.is_actively_capturing;
        return backreference_position < other.backreference_position;
    }
};

struct ActiveNFAState
{
    std::shared_ptr<NFAState> nfa_state;
    CaptureGroupInfo capture_info;

    bool operator<(const ActiveNFAState &other) const
    {
        if (nfa_state != other.nfa_state)
            return nfa_state < other.nfa_state;
        return capture_info < other.capture_info;
    }
};

using ActiveStateList = std::vector<ActiveNFAState>;

bool has_matching_state(const ActiveStateList &active_states)
{
    return std::any_of(active_states.begin(), active_states.end(), [](const ActiveNFAState &state)
                       { return state.nfa_state->character_code == OPCODE_MATCHED; });
}

void add_state_with_epsilon_closure(std::shared_ptr<NFAState> state_to_add,
                                    CaptureGroupInfo capture_info,
                                    ActiveStateList &active_states,
                                    std::set<std::shared_ptr<NFAState>> &visited_states)
{
    if (!state_to_add || visited_states.count(state_to_add))
        return;
    visited_states.insert(state_to_add);

    if (state_to_add->capture_group_start >= 0)
    {
        int group_id = state_to_add->capture_group_start;
        capture_info.captured_text[group_id].clear();
        capture_info.is_actively_capturing[group_id] = true;
    }
    if (state_to_add->capture_group_end >= 0)
    {
        int group_id = state_to_add->capture_group_end;
        capture_info.is_actively_capturing[group_id] = false;
    }

    if (state_to_add->character_code == OPCODE_SPLIT)
    {
        add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, active_states, visited_states);
        add_state_with_epsilon_closure(state_to_add->alternative_transition, capture_info, active_states, visited_states);
        return;
    }

    active_states.push_back({state_to_add, capture_info});
}

void initialize_active_states(std::shared_ptr<NFAState> start_state, ActiveStateList &active_states)
{
    active_states.clear();
    std::set<std::shared_ptr<NFAState>> visited_states;
    CaptureGroupInfo initial_capture_info{};
    add_state_with_epsilon_closure(start_state, initial_capture_info, active_states, visited_states);
}

void process_character_step(ActiveStateList &current_states, char input_char, ActiveStateList &next_states)
{
    next_states.clear();
    profiler.total_steps++; // Count each step

    for (const ActiveNFAState &active_state : current_states)
    {
        auto nfa_state = active_state.nfa_state;
        auto capture_info = active_state.capture_info;

        bool can_consume_character = false;
        bool character_added_by_backref = false;

        switch (nfa_state->character_code)
        {
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
            can_consume_character = (std::find(nfa_state->character_set.begin(), nfa_state->character_set.end(), input_char) != nfa_state->character_set.end());
            break;
        case OPCODE_MATCH_ANTI_CHOICE:
            can_consume_character = (std::find(nfa_state->character_set.begin(), nfa_state->character_set.end(), input_char) == nfa_state->character_set.end());
            break;
        default:
            if (nfa_state->character_code == input_char)
                can_consume_character = true;
            break;
        }

        if (can_consume_character)
        {
            for (auto &[group_id, is_active] : capture_info.is_actively_capturing)
                if (is_active)
                    capture_info.captured_text[group_id].push_back(input_char);

            std::set<std::shared_ptr<NFAState>> visited_states;
            add_state_with_epsilon_closure(nfa_state->primary_transition, capture_info, next_states, visited_states);
        }
    }

    profiler.total_states_visited += current_states.size();
    profiler.max_active_states = std::max(profiler.max_active_states, current_states.size());
}

// --- MatchInfo + Matching Functions ---
struct MatchInfo
{
    bool found;
    size_t start_pos;
    size_t end_pos;
};

MatchInfo match_text_with_positions(std::shared_ptr<NFAState> nfa_start_state, std::string_view input_text)
{
    ActiveStateList current_states, next_states;
    initialize_active_states(nfa_start_state, current_states);
    size_t match_start = 0;
    profiler.lines_processed++;

    for (size_t i = 0; i <= input_text.size(); ++i)
    {
        if (has_matching_state(current_states))
            return {true, match_start, i};
        if (i == input_text.size())
            break;
        process_character_step(current_states, input_text[i], next_states);
        current_states.swap(next_states);
    }
    return {false, 0, 0};
}

// --- Output ---
void print_with_color(const std::string &line, const MatchInfo &match_info, bool use_color)
{
    if (!use_color || !match_info.found)
    {
        std::cout << line << '\n';
        return;
    }
    std::cout << line.substr(0, match_info.start_pos)
              << COLOR_RED_BOLD
              << line.substr(match_info.start_pos, match_info.end_pos - match_info.start_pos)
              << COLOR_RESET
              << line.substr(match_info.end_pos)
              << '\n';
}

// --- Main ---
int main(int argc, char *argv[])
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    bool use_recursive_search = false;
    bool use_color = true;
    std::string regex_pattern_string;
    std::vector<std::string> target_files;

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "-E")
        {
            if (i + 1 < argc)
            {
                regex_pattern_string = argv[++i];
            }
            else
            {
                std::cerr << "Error: -E requires a pattern.\n";
                return 1;
            }
        }
        else if (arg == "-r")
        {
            use_recursive_search = true;
        }
        else if (arg.find("--color=") == 0)
        {
            std::string opt = arg.substr(8);
            use_color = (opt != "never");
        }
        else if (arg == "--profile")
        {
            enable_profiling = true;
        }
        else
        {
            target_files.push_back(arg);
        }
    }

    std::shared_ptr<NFAState> nfa;
    try
    {
        nfa = compile_regex_to_nfa(regex_pattern_string);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Regex error: " << e.what() << '\n';
        return 1;
    }

    bool found_any = false;

    if (target_files.empty())
    {
        std::string line;
        while (std::getline(std::cin, line))
        {
            MatchInfo mi = match_text_with_positions(nfa, line);
            if (mi.found)
            {
                print_with_color(line, mi, use_color);
                found_any = true;
            }
        }
    }
    else
    {
        for (const auto &f : target_files)
        {
            std::ifstream fin(f);
            if (!fin.is_open())
                continue;
            std::string line;
            while (std::getline(fin, line))
            {
                MatchInfo mi = match_text_with_positions(nfa, line);
                if (mi.found)
                {
                    print_with_color(line, mi, use_color);
                    found_any = true;
                }
            }
        }
    }

    if (enable_profiling)
    {
        std::cerr << "\n[Regex Profiler Summary]\n"
                  << "  Lines processed      : " << profiler.lines_processed << "\n"
                  << "  Total simulation steps: " << profiler.total_steps << "\n"
                  << "  Total states visited : " << profiler.total_states_visited << "\n"
                  << "  Max active states     : " << profiler.max_active_states << "\n";
    }

    return !found_any;
}