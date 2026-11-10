#include "grep_engine.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include <vector>
#include <string>
#include <set>
#include <map>
#include <algorithm>
#include <memory>
#include <new>

/* =========================================================================
 * Internal NFA Structures and Opcodes
 * ========================================================================= */

enum RegexOpcodes {
    OPCODE_SPLIT = 256,
    OPCODE_MATCH_ANY,
    OPCODE_MATCH_WORD,
    OPCODE_MATCH_DIGIT,
    OPCODE_MATCH_SPACE,
    OPCODE_MATCH_CHOICE,
    OPCODE_MATCH_ANTI_CHOICE,
    OPCODE_MATCH_START,
    OPCODE_MATCH_END,
    OPCODE_BACKREF_START = 300,
    OPCODE_MATCHED = 1000
};

struct NFAState {
    std::shared_ptr<NFAState> primary_transition;
    std::shared_ptr<NFAState> alternative_transition;
    int character_code;
    std::vector<int> character_set;
    int capture_group_start;
    int capture_group_end;

    NFAState()
        : character_code(-1),
          capture_group_start(-1),
          capture_group_end(-1) {}
};

struct NFAFragment {
    std::shared_ptr<NFAState> start_state;
    std::vector<std::shared_ptr<NFAState>*> dangling_outputs;
};

struct PatternCursor {
    const char *str;
    size_t pos;
    size_t len;
    int next_capture_group_id;

    PatternCursor(const char *s, size_t l)
        : str(s), pos(0), len(l), next_capture_group_id(1) {}

    bool empty() const { return pos >= len; }
    char front() const { return pos < len ? str[pos] : '\0'; }
    void remove_prefix(size_t n) { pos = (pos + n <= len) ? pos + n : len; }
};

/* Forward declarations */
static NFAFragment parse_regex_internal(PatternCursor &cursor, NFAFragment left_fragment, int min_precedence, char *err_buf, size_t err_buf_len, int &err_code);
static NFAFragment parse_primary_element_internal(PatternCursor &cursor, char *err_buf, size_t err_buf_len, int &err_code);

static void set_error(char *err_buf, size_t err_buf_len, int code, int &out_code, const char *msg) {
    out_code = code;
    if (err_buf && err_buf_len > 0) {
        snprintf(err_buf, err_buf_len, "%s", msg);
    }
}

static NFAFragment parse_primary_element_internal(PatternCursor &cursor, char *err_buf, size_t err_buf_len, int &err_code) {
    if (cursor.empty()) {
        set_error(err_buf, err_buf_len, GREP_ERR_UNEXPECTED_END, err_code, "Unexpected end of pattern");
        return NFAFragment();
    }

    std::shared_ptr<NFAState> new_state = std::make_shared<NFAState>();
    NFAFragment current_fragment;
    current_fragment.start_state = new_state;
    current_fragment.dangling_outputs.push_back(&new_state->primary_transition);

    char current_char = cursor.front();
    cursor.remove_prefix(1);

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
        if (cursor.empty()) {
            set_error(err_buf, err_buf_len, GREP_ERR_UNEXPECTED_END, err_code, "Unexpected end of pattern after backslash");
            return NFAFragment();
        }
        current_char = cursor.front();
        cursor.remove_prefix(1);
        if (current_char == 'd') {
            new_state->character_code = OPCODE_MATCH_DIGIT;
        } else if (current_char == 'w') {
            new_state->character_code = OPCODE_MATCH_WORD;
        } else if (current_char == 's') {
            new_state->character_code = OPCODE_MATCH_SPACE;
        } else if (isdigit(static_cast<unsigned char>(current_char))) {
            new_state->character_code = OPCODE_BACKREF_START + (current_char - '0');
        } else {
            new_state->character_code = static_cast<unsigned char>(current_char);
        }
        break;
    }
    case '[': {
        bool is_negated = false;
        if (!cursor.empty() && cursor.front() == '^') {
            is_negated = true;
            cursor.remove_prefix(1);
        }
        new_state->character_code = is_negated ? OPCODE_MATCH_ANTI_CHOICE : OPCODE_MATCH_CHOICE;

        /* Support closing bracket as first literal character if present: []a] or [^]a] */
        if (!cursor.empty() && cursor.front() == ']') {
            new_state->character_set.push_back(static_cast<unsigned char>(']'));
            cursor.remove_prefix(1);
        }

        while (!cursor.empty() && cursor.front() != ']') {
            new_state->character_set.push_back(static_cast<unsigned char>(cursor.front()));
            cursor.remove_prefix(1);
        }
        if (cursor.empty()) {
            set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_BRACKET, err_code, "Unclosed bracket expression");
            return NFAFragment();
        }
        cursor.remove_prefix(1); /* consume ']' */
        break;
    }
    case '(': {
        int current_group_id = cursor.next_capture_group_id++;
        std::shared_ptr<NFAState> capture_start_state = std::make_shared<NFAState>();
        capture_start_state->character_code = OPCODE_SPLIT;
        capture_start_state->capture_group_start = current_group_id;

        if (cursor.empty()) {
            set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_GROUP, err_code, "Expected pattern inside group");
            return NFAFragment();
        }

        /* Check for empty group () */
        if (cursor.front() == ')') {
            cursor.remove_prefix(1);
            std::shared_ptr<NFAState> capture_end_state = std::make_shared<NFAState>();
            capture_end_state->character_code = OPCODE_SPLIT;
            capture_end_state->capture_group_end = current_group_id;
            capture_start_state->primary_transition = capture_end_state;
            current_fragment.start_state = capture_start_state;
            current_fragment.dangling_outputs.clear();
            current_fragment.dangling_outputs.push_back(&capture_end_state->primary_transition);
            break;
        }

        NFAFragment inner_first = parse_primary_element_internal(cursor, err_buf, err_buf_len, err_code);
        if (err_code != GREP_OK) return NFAFragment();

        NFAFragment inner_fragment = parse_regex_internal(cursor, inner_first, 0, err_buf, err_buf_len, err_code);
        if (err_code != GREP_OK) return NFAFragment();

        if (cursor.empty() || cursor.front() != ')') {
            set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_GROUP, err_code, "Expected ')' to close group");
            return NFAFragment();
        }
        cursor.remove_prefix(1); /* consume ')' */

        std::shared_ptr<NFAState> capture_end_state = std::make_shared<NFAState>();
        capture_end_state->character_code = OPCODE_SPLIT;
        capture_end_state->capture_group_end = current_group_id;

        capture_start_state->primary_transition = inner_fragment.start_state;
        for (size_t d = 0; d < inner_fragment.dangling_outputs.size(); ++d) {
            *inner_fragment.dangling_outputs[d] = capture_end_state;
        }

        current_fragment.start_state = capture_start_state;
        current_fragment.dangling_outputs.clear();
        current_fragment.dangling_outputs.push_back(&capture_end_state->primary_transition);
        break;
    }
    default:
        new_state->character_code = static_cast<unsigned char>(current_char);
        break;
    }

    /* Process post-fix quantifiers */
    if (!cursor.empty()) {
        char q = cursor.front();
        if (q == '*') {
            std::shared_ptr<NFAState> split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = current_fragment.start_state;
            for (size_t d = 0; d < current_fragment.dangling_outputs.size(); ++d) {
                *current_fragment.dangling_outputs[d] = split_state;
            }
            current_fragment.start_state = split_state;
            current_fragment.dangling_outputs.clear();
            current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
            cursor.remove_prefix(1);
        } else if (q == '+') {
            std::shared_ptr<NFAState> split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = current_fragment.start_state;
            for (size_t d = 0; d < current_fragment.dangling_outputs.size(); ++d) {
                *current_fragment.dangling_outputs[d] = split_state;
            }
            current_fragment.dangling_outputs.clear();
            current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
            cursor.remove_prefix(1);
        } else if (q == '?') {
            std::shared_ptr<NFAState> split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = current_fragment.start_state;
            current_fragment.start_state = split_state;
            current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
            cursor.remove_prefix(1);
        }
    }

    return current_fragment;
}

static inline int get_precedence(char op) {
    if (op == '|') return 0;
    if (op == ']' || op == ')') return -1;
    return 1;
}

static NFAFragment parse_regex_internal(PatternCursor &cursor, NFAFragment left_fragment, int min_precedence, char *err_buf, size_t err_buf_len, int &err_code) {
    char lookahead = !cursor.empty() ? cursor.front() : '\0';

    while (!cursor.empty() && get_precedence(lookahead) >= min_precedence) {
        char current_op = lookahead;
        if (current_op == '|') {
            cursor.remove_prefix(1);
        }

        NFAFragment right_fragment = parse_primary_element_internal(cursor, err_buf, err_buf_len, err_code);
        if (err_code != GREP_OK) return left_fragment;

        lookahead = !cursor.empty() ? cursor.front() : '\0';

        while (!cursor.empty() && get_precedence(lookahead) > get_precedence(current_op)) {
            right_fragment = parse_regex_internal(cursor, right_fragment, get_precedence(current_op) + 1, err_buf, err_buf_len, err_code);
            if (err_code != GREP_OK) return left_fragment;
            lookahead = !cursor.empty() ? cursor.front() : '\0';
        }

        if (current_op == '|') {
            std::shared_ptr<NFAState> split_state = std::make_shared<NFAState>();
            split_state->character_code = OPCODE_SPLIT;
            split_state->primary_transition = left_fragment.start_state;
            split_state->alternative_transition = right_fragment.start_state;

            left_fragment.start_state = split_state;
            left_fragment.dangling_outputs.insert(left_fragment.dangling_outputs.end(),
                                                  right_fragment.dangling_outputs.begin(),
                                                  right_fragment.dangling_outputs.end());
        } else {
            for (size_t d = 0; d < left_fragment.dangling_outputs.size(); ++d) {
                *left_fragment.dangling_outputs[d] = right_fragment.start_state;
            }
            left_fragment.dangling_outputs = right_fragment.dangling_outputs;
        }

        lookahead = !cursor.empty() ? cursor.front() : '\0';
    }

    return left_fragment;
}

/* =========================================================================
 * Regex Handle Definition
 * ========================================================================= */

struct grep_regex {
    std::shared_ptr<NFAState> start_state;
    bool is_anchored_start;
    bool is_anchored_end;
    bool matches_empty_string;
    int max_capture_group;
};

/* =========================================================================
 * C API Compilation Implementation
 * ========================================================================= */

int grep_compile_len(const char *pattern, size_t pattern_len, grep_regex_t **out_regex, char *err_buf, size_t err_buf_len) {
    if (!out_regex) return GREP_ERR_NULL_ARG;
    *out_regex = NULL;
    if (!pattern && pattern_len > 0) {
        set_error(err_buf, err_buf_len, GREP_ERR_NULL_ARG, *(int*)&pattern_len, "Pattern pointer is NULL");
        return GREP_ERR_NULL_ARG;
    }

    if (err_buf && err_buf_len > 0) {
        err_buf[0] = '\0';
    }

    try {
        std::shared_ptr<NFAState> matched_state = std::make_shared<NFAState>();
        matched_state->character_code = OPCODE_MATCHED;

        grep_regex_t *re = new grep_regex_t();
        re->start_state = matched_state;
        re->is_anchored_start = false;
        re->is_anchored_end = false;
        re->matches_empty_string = (pattern_len == 0);
        re->max_capture_group = 0;

        if (pattern_len == 0) {
            *out_regex = re;
            return GREP_OK;
        }

        PatternCursor cursor(pattern, pattern_len);
        int err_code = GREP_OK;

        NFAFragment first_fragment = parse_primary_element_internal(cursor, err_buf, err_buf_len, err_code);
        if (err_code != GREP_OK) {
            delete re;
            return err_code;
        }

        NFAFragment complete_fragment = parse_regex_internal(cursor, first_fragment, 0, err_buf, err_buf_len, err_code);
        if (err_code != GREP_OK) {
            delete re;
            return err_code;
        }

        if (!cursor.empty()) {
            char trailing = cursor.front();
            delete re;
            if (trailing == ')') {
                set_error(err_buf, err_buf_len, GREP_ERR_UNMATCHED_PAREN, err_code, "Unmatched ')' in pattern");
                return GREP_ERR_UNMATCHED_PAREN;
            }
            if (trailing == ']') {
                set_error(err_buf, err_buf_len, GREP_ERR_UNMATCHED_BRACKET, err_code, "Unmatched ']' in pattern");
                return GREP_ERR_UNMATCHED_BRACKET;
            }
            set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, err_code, "Syntax error in regular expression");
            return GREP_ERR_SYNTAX;
        }

        for (size_t d = 0; d < complete_fragment.dangling_outputs.size(); ++d) {
            *complete_fragment.dangling_outputs[d] = matched_state;
        }

        re->start_state = complete_fragment.start_state;
        re->max_capture_group = cursor.next_capture_group_id - 1;
        re->is_anchored_start = (re->start_state->character_code == OPCODE_MATCH_START);

        *out_regex = re;
        return GREP_OK;
    } catch (const std::bad_alloc &) {
        set_error(err_buf, err_buf_len, GREP_ERR_NOMEM, *(int*)out_regex, "Out of memory");
        return GREP_ERR_NOMEM;
    } catch (...) {
        set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, *(int*)out_regex, "Unknown error during regex compilation");
        return GREP_ERR_SYNTAX;
    }
}

int grep_compile(const char *pattern, grep_regex_t **out_regex, char *err_buf, size_t err_buf_len) {
    if (!pattern) return grep_compile_len(NULL, 0, out_regex, err_buf, err_buf_len);
    return grep_compile_len(pattern, strlen(pattern), out_regex, err_buf, err_buf_len);
}

void grep_free(grep_regex_t *regex) {
    if (regex) {
        delete regex;
    }
}

/* =========================================================================
 * NFA Simulation Engine
 * ========================================================================= */

struct ActiveCaptureInfo {
    grep_span_t captured_spans[GREP_MAX_CAPTURE_GROUPS];
    bool is_actively_capturing[GREP_MAX_CAPTURE_GROUPS];
    size_t backreference_offset[GREP_MAX_CAPTURE_GROUPS];

    ActiveCaptureInfo() {
        for (int i = 0; i < GREP_MAX_CAPTURE_GROUPS; ++i) {
            captured_spans[i].start_pos = 0;
            captured_spans[i].end_pos = 0;
            captured_spans[i].valid = 0;
            is_actively_capturing[i] = false;
            backreference_offset[i] = 0;
        }
    }

    bool operator<(const ActiveCaptureInfo &other) const {
        for (int i = 1; i < GREP_MAX_CAPTURE_GROUPS; ++i) {
            if (captured_spans[i].valid != other.captured_spans[i].valid)
                return captured_spans[i].valid < other.captured_spans[i].valid;
            if (captured_spans[i].start_pos != other.captured_spans[i].start_pos)
                return captured_spans[i].start_pos < other.captured_spans[i].start_pos;
            if (captured_spans[i].end_pos != other.captured_spans[i].end_pos)
                return captured_spans[i].end_pos < other.captured_spans[i].end_pos;
            if (is_actively_capturing[i] != other.is_actively_capturing[i])
                return is_actively_capturing[i] < other.is_actively_capturing[i];
            if (backreference_offset[i] != other.backreference_offset[i])
                return backreference_offset[i] < other.backreference_offset[i];
        }
        return false;
    }
};

struct ActiveNFAState {
    std::shared_ptr<NFAState> nfa_state;
    ActiveCaptureInfo capture_info;

    bool operator<(const ActiveNFAState &other) const {
        if (nfa_state != other.nfa_state)
            return nfa_state < other.nfa_state;
        return capture_info < other.capture_info;
    }
};

typedef std::vector<ActiveNFAState> ActiveStateList;

static void add_state_with_epsilon_closure(
    const std::shared_ptr<NFAState> &state_to_add,
    ActiveCaptureInfo capture_info,
    size_t current_pos,
    ActiveStateList &active_states,
    std::set<std::shared_ptr<NFAState> > &visited_states)
{
    if (!state_to_add || visited_states.count(state_to_add)) return;
    visited_states.insert(state_to_add);

    if (state_to_add->capture_group_start >= 1 && state_to_add->capture_group_start < GREP_MAX_CAPTURE_GROUPS) {
        int gid = state_to_add->capture_group_start;
        capture_info.captured_spans[gid].start_pos = current_pos;
        capture_info.captured_spans[gid].end_pos = current_pos;
        capture_info.captured_spans[gid].valid = 1;
        capture_info.is_actively_capturing[gid] = true;
    }
    if (state_to_add->capture_group_end >= 1 && state_to_add->capture_group_end < GREP_MAX_CAPTURE_GROUPS) {
        int gid = state_to_add->capture_group_end;
        capture_info.captured_spans[gid].end_pos = current_pos;
        capture_info.is_actively_capturing[gid] = false;
    }

    if (state_to_add->character_code == OPCODE_SPLIT) {
        add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, current_pos, active_states, visited_states);
        add_state_with_epsilon_closure(state_to_add->alternative_transition, capture_info, current_pos, active_states, visited_states);
        return;
    }

    active_states.push_back({state_to_add, capture_info});
}

static void initialize_active_states(const std::shared_ptr<NFAState> &start_state, size_t pos, ActiveStateList &active_states) {
    active_states.clear();
    std::set<std::shared_ptr<NFAState> > visited_states;
    ActiveCaptureInfo initial_capture;
    add_state_with_epsilon_closure(start_state, initial_capture, pos, active_states, visited_states);
}

static bool check_has_matching_state(const ActiveStateList &states, ActiveCaptureInfo *out_caps = NULL) {
    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i].nfa_state->character_code == OPCODE_MATCHED) {
            if (out_caps) {
                *out_caps = states[i].capture_info;
            }
            return true;
        }
    }
    return false;
}

static void process_character_step(
    const ActiveStateList &current_states,
    char input_char,
    size_t next_char_pos,
    const char *full_text,
    ActiveStateList &next_states,
    grep_profiler_t *profiler)
{
    next_states.clear();
    if (profiler) {
        profiler->total_steps++;
    }

    for (size_t s = 0; s < current_states.size(); ++s) {
        const ActiveNFAState &cur = current_states[s];
        const std::shared_ptr<NFAState> &nfa_state = cur.nfa_state;
        ActiveCaptureInfo capture_info = cur.capture_info;

        bool can_consume = false;
        bool character_added_by_backref = false;

        switch (nfa_state->character_code) {
        case OPCODE_MATCH_ANY:
            can_consume = true;
            break;
        case OPCODE_MATCH_DIGIT:
            can_consume = (isdigit(static_cast<unsigned char>(input_char)) != 0);
            break;
        case OPCODE_MATCH_WORD:
            can_consume = (isalnum(static_cast<unsigned char>(input_char)) != 0 || input_char == '_');
            break;
        case OPCODE_MATCH_SPACE:
            can_consume = (isspace(static_cast<unsigned char>(input_char)) != 0);
            break;
        case OPCODE_MATCH_CHOICE: {
            for (size_t ci = 0; ci < nfa_state->character_set.size(); ++ci) {
                if (nfa_state->character_set[ci] == static_cast<unsigned char>(input_char)) {
                    can_consume = true;
                    break;
                }
            }
            break;
        }
        case OPCODE_MATCH_ANTI_CHOICE: {
            bool found = false;
            for (size_t ci = 0; ci < nfa_state->character_set.size(); ++ci) {
                if (nfa_state->character_set[ci] == static_cast<unsigned char>(input_char)) {
                    found = true;
                    break;
                }
            }
            can_consume = !found;
            break;
        }
        default:
            if (nfa_state->character_code >= OPCODE_BACKREF_START && nfa_state->character_code < OPCODE_BACKREF_START + GREP_MAX_CAPTURE_GROUPS) {
                int gid = nfa_state->character_code - OPCODE_BACKREF_START;
                if (capture_info.captured_spans[gid].valid) {
                    size_t c_start = capture_info.captured_spans[gid].start_pos;
                    size_t c_end = capture_info.captured_spans[gid].end_pos;
                    size_t c_len = (c_end >= c_start) ? (c_end - c_start) : 0;
                    if (c_len > 0 && full_text) {
                        size_t offset = capture_info.backreference_offset[gid];
                        if (offset < c_len && full_text[c_start + offset] == input_char) {
                            offset++;
                            for (int i = 1; i < GREP_MAX_CAPTURE_GROUPS; ++i) {
                                if (capture_info.is_actively_capturing[i]) {
                                    capture_info.captured_spans[i].end_pos = next_char_pos;
                                }
                            }
                            character_added_by_backref = true;
                            if (offset == c_len) {
                                capture_info.backreference_offset[gid] = 0;
                                can_consume = true;
                            } else {
                                capture_info.backreference_offset[gid] = offset;
                                next_states.push_back({nfa_state, capture_info});
                                continue;
                            }
                        }
                    }
                }
            } else if (nfa_state->character_code == static_cast<unsigned char>(input_char)) {
                can_consume = true;
            }
            break;
        }

        if (can_consume) {
            if (!character_added_by_backref) {
                for (int i = 1; i < GREP_MAX_CAPTURE_GROUPS; ++i) {
                    if (capture_info.is_actively_capturing[i]) {
                        capture_info.captured_spans[i].end_pos = next_char_pos;
                    }
                }
            }

            std::set<std::shared_ptr<NFAState> > visited;
            add_state_with_epsilon_closure(nfa_state->primary_transition, capture_info, next_char_pos, next_states, visited);
        }
    }

    if (profiler) {
        profiler->total_states_visited += current_states.size();
        if (current_states.size() > profiler->max_active_states) {
            profiler->max_active_states = current_states.size();
        }
    }
}

/* =========================================================================
 * Matching and Searching Implementation
 * ========================================================================= */

int grep_find_all_len(const grep_regex_t *regex, const char *text, size_t text_len, grep_match_result_t *out_result, grep_profiler_t *profiler) {
    if (!regex || !out_result) return GREP_ERR_NULL_ARG;
    if (!text && text_len > 0) return GREP_ERR_NULL_ARG;

    out_result->found = 0;
    out_result->matches = NULL;
    out_result->match_count = 0;
    out_result->match_capacity = 0;

    if (profiler) {
        profiler->lines_processed++;
    }

    /* Handle empty regex or regex that matches empty strings */
    if (regex->matches_empty_string) {
        out_result->found = 1;
        out_result->matches = (grep_single_match_t *)calloc(1, sizeof(grep_single_match_t));
        if (!out_result->matches) return GREP_ERR_NOMEM;
        out_result->match_count = 1;
        out_result->match_capacity = 1;
        out_result->matches[0].start_pos = 0;
        out_result->matches[0].end_pos = 0;
        return GREP_OK;
    }

    std::vector<grep_single_match_t> collected_matches;
    size_t current_global_pos = 0;

    bool is_anchored_start = regex->is_anchored_start;

    while (current_global_pos <= text_len) {
        size_t remaining_len = text_len - current_global_pos;
        const char *remaining_text = (text_len > 0 && current_global_pos < text_len) ? (text + current_global_pos) : "";

        ActiveStateList current_states, next_states;

        if (is_anchored_start) {
            if (current_global_pos > 0) {
                break; /* Anchored pattern can only match at index 0 */
            }
            std::set<std::shared_ptr<NFAState> > visited;
            ActiveCaptureInfo init_caps;
            add_state_with_epsilon_closure(regex->start_state->primary_transition, init_caps, 0, current_states, visited);
        } else {
            initialize_active_states(regex->start_state, current_global_pos, current_states);
        }

        bool match_found_in_segment = false;
        size_t match_length = 0;
        ActiveCaptureInfo winning_caps;

        for (size_t i = 0; i <= remaining_len; ++i) {
            /* Check if end-of-string anchor '$' can transition */
            if (i == remaining_len) {
                ActiveStateList eos_states;
                for (size_t s = 0; s < current_states.size(); ++s) {
                    if (current_states[s].nfa_state->character_code == OPCODE_MATCH_END) {
                        std::set<std::shared_ptr<NFAState> > visited;
                        add_state_with_epsilon_closure(
                            current_states[s].nfa_state->primary_transition,
                            current_states[s].capture_info,
                            current_global_pos + i,
                            eos_states,
                            visited);
                    } else {
                        eos_states.push_back(current_states[s]);
                    }
                }
                current_states.swap(eos_states);
            }

            if (check_has_matching_state(current_states, &winning_caps)) {
                match_found_in_segment = true;
                match_length = i;
                break;
            }

            if (i == remaining_len) {
                break;
            }

            char input_char = remaining_text[i];
            process_character_step(current_states, input_char, current_global_pos + i + 1, text, next_states, profiler);
            current_states.swap(next_states);
        }

        if (match_found_in_segment) {
            grep_single_match_t sm;
            memset(&sm, 0, sizeof(sm));
            sm.start_pos = current_global_pos;
            sm.end_pos = current_global_pos + match_length;
            for (int cg = 0; cg < GREP_MAX_CAPTURE_GROUPS; ++cg) {
                sm.captures[cg] = winning_caps.captured_spans[cg];
            }
            collected_matches.push_back(sm);

            current_global_pos += (match_length > 0) ? match_length : 1;
            if (is_anchored_start) {
                break;
            }
        } else {
            if (is_anchored_start) {
                break;
            }
            if (current_global_pos < text_len) {
                current_global_pos++;
            } else {
                break;
            }
        }
    }

    if (!collected_matches.empty()) {
        out_result->found = 1;
        out_result->match_count = collected_matches.size();
        out_result->match_capacity = collected_matches.size();
        out_result->matches = (grep_single_match_t *)malloc(sizeof(grep_single_match_t) * collected_matches.size());
        if (!out_result->matches) return GREP_ERR_NOMEM;
        memcpy(out_result->matches, &collected_matches[0], sizeof(grep_single_match_t) * collected_matches.size());
    } else {
        out_result->found = 0;
        out_result->matches = NULL;
        out_result->match_count = 0;
    }

    return GREP_OK;
}

int grep_find_all(const grep_regex_t *regex, const char *text, grep_match_result_t *out_result, grep_profiler_t *profiler) {
    if (!text) return grep_find_all_len(regex, NULL, 0, out_result, profiler);
    return grep_find_all_len(regex, text, strlen(text), out_result, profiler);
}

int grep_match_len(const grep_regex_t *regex, const char *text, size_t text_len, grep_profiler_t *profiler) {
    grep_match_result_t res;
    int rc = grep_find_all_len(regex, text, text_len, &res, profiler);
    if (rc != GREP_OK) return rc;
    int found = res.found;
    grep_match_result_free(&res);
    return found;
}

int grep_match(const grep_regex_t *regex, const char *text) {
    if (!text) return grep_match_len(regex, NULL, 0, NULL);
    return grep_match_len(regex, text, strlen(text), NULL);
}

void grep_match_result_free(grep_match_result_t *result) {
    if (result) {
        if (result->matches) {
            free(result->matches);
            result->matches = NULL;
        }
        result->match_count = 0;
        result->match_capacity = 0;
        result->found = 0;
    }
}

void grep_profiler_reset(grep_profiler_t *profiler) {
    if (profiler) {
        profiler->total_steps = 0;
        profiler->total_states_visited = 0;
        profiler->max_active_states = 0;
        profiler->lines_processed = 0;
    }
}
