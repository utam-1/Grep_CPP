#include "grep_engine.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include <vector>
#include <string>
#include <set>
#include <algorithm>
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
    int id;
    NFAState *primary_transition;
    NFAState *alternative_transition;
    int character_code;
    unsigned char char_bitmap[32]; /* 256-bit lookup table for [...] and [^...] */
    int capture_group_start;
    int capture_group_end;

    NFAState()
        : id(-1),
          primary_transition(NULL),
          alternative_transition(NULL),
          character_code(-1),
          capture_group_start(-1),
          capture_group_end(-1) {
        memset(char_bitmap, 0, sizeof(char_bitmap));
    }

    void set_char_bit(unsigned char ch) {
        char_bitmap[ch >> 3] |= static_cast<unsigned char>(1u << (ch & 7));
    }

    bool has_char_bit(unsigned char ch) const {
        return (char_bitmap[ch >> 3] & static_cast<unsigned char>(1u << (ch & 7))) != 0;
    }
};

struct NFAFragment {
    NFAState *start_state;
    std::vector<NFAState **> dangling_outputs;

    NFAFragment() : start_state(NULL) {}
};

struct PatternCursor {
    const char *str;
    size_t pos;
    size_t len;
    int next_capture_group_id;
    std::vector<NFAState *> *arena;

    PatternCursor(const char *s, size_t l, std::vector<NFAState *> *a)
        : str(s), pos(0), len(l), next_capture_group_id(1), arena(a) {}

    bool empty() const { return pos >= len; }
    char front() const { return pos < len ? str[pos] : '\0'; }
    void remove_prefix(size_t n) { pos = (pos + n <= len) ? pos + n : len; }

    NFAState *alloc_state() {
        arena->push_back(NULL);
        NFAState *s = new NFAState();
        s->id = static_cast<int>(arena->size()) - 1;
        arena->back() = s;
        return s;
    }
};

/* Forward declarations */
static NFAFragment parse_regex_internal(PatternCursor &cursor, NFAFragment left_fragment, int min_precedence, char *err_buf, size_t err_buf_len, int &err_code);
static NFAFragment parse_primary_element_internal(PatternCursor &cursor, char *err_buf, size_t err_buf_len, int &err_code);

static int set_error(char *err_buf, size_t err_buf_len, int code, const char *msg) {
    if (err_buf && err_buf_len > 0) {
        err_buf[0] = '\0';
        if (msg) {
            strncpy(err_buf, msg, err_buf_len - 1);
            err_buf[err_buf_len - 1] = '\0';
        }
    }
    return code;
}

static void add_char_class_shorthand(NFAState *state, char shorthand) {
    for (int c = 0; c < 256; ++c) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (shorthand == 'd' && isdigit(uc)) {
            state->set_char_bit(uc);
        } else if (shorthand == 'w' && (isalnum(uc) || uc == '_')) {
            state->set_char_bit(uc);
        } else if (shorthand == 's' && isspace(uc)) {
            state->set_char_bit(uc);
        }
    }
}

static bool parse_bracket_atom(PatternCursor &cursor, unsigned char &out_char, bool &is_shorthand, char &shorthand_type, char *err_buf, size_t err_buf_len, int &err_code) {
    is_shorthand = false;
    shorthand_type = '\0';
    if (cursor.empty()) {
        err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_BRACKET, "Unclosed bracket expression");
        return false;
    }
    char ch = cursor.front();
    cursor.remove_prefix(1);

    if (ch == '\\') {
        if (cursor.empty()) {
            err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_BRACKET, "Unclosed bracket expression after backslash");
            return false;
        }
        char esc = cursor.front();
        cursor.remove_prefix(1);
        if (esc == 'd' || esc == 'w' || esc == 's') {
            is_shorthand = true;
            shorthand_type = esc;
            return true;
        } else if (esc == 'n') {
            out_char = '\n';
            return true;
        } else if (esc == 't') {
            out_char = '\t';
            return true;
        } else if (esc == 'r') {
            out_char = '\r';
            return true;
        }
        out_char = static_cast<unsigned char>(esc);
        return true;
    }

    out_char = static_cast<unsigned char>(ch);
    return true;
}

static NFAFragment parse_primary_element_internal(PatternCursor &cursor, char *err_buf, size_t err_buf_len, int &err_code) {
    if (cursor.empty()) {
        err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNEXPECTED_END, "Unexpected end of pattern");
        return NFAFragment();
    }

    char current_char = cursor.front();

    if (current_char == '*' || current_char == '+' || current_char == '?') {
        err_code = set_error(err_buf, err_buf_len, GREP_ERR_INVALID_QUANTIFIER, "Quantifier without preceding expression");
        return NFAFragment();
    }
    if (current_char == ')') {
        err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNMATCHED_PAREN, "Unmatched ')' in pattern");
        return NFAFragment();
    }
    if (current_char == '|') {
        err_code = set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, "Unexpected '|' in pattern");
        return NFAFragment();
    }

    NFAState *new_state = cursor.alloc_state();
    NFAFragment current_fragment;
    current_fragment.start_state = new_state;
    current_fragment.dangling_outputs.push_back(&new_state->primary_transition);

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
            err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNEXPECTED_END, "Unexpected end of pattern after backslash");
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
        } else if (current_char == 'n') {
            new_state->character_code = '\n';
        } else if (current_char == 't') {
            new_state->character_code = '\t';
        } else if (current_char == 'r') {
            new_state->character_code = '\r';
        } else if (current_char >= '1' && current_char <= '9') {
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

        bool first_in_bracket = true;
        bool has_prev_char = false;
        unsigned char prev_char = 0;

        while (!cursor.empty()) {
            if (cursor.front() == ']' && !first_in_bracket) {
                break;
            }
            first_in_bracket = false;

            /* Check for range operator '-' between two literal characters */
            if (cursor.front() == '-' && has_prev_char) {
                /* Peek ahead to see if '-' is at the end before ']' */
                if (cursor.pos + 1 < cursor.len && cursor.str[cursor.pos + 1] != ']') {
                    cursor.remove_prefix(1); /* consume '-' */
                    unsigned char end_char = 0;
                    bool end_is_shorthand = false;
                    char end_shorthand = '\0';
                    if (!parse_bracket_atom(cursor, end_char, end_is_shorthand, end_shorthand, err_buf, err_buf_len, err_code)) {
                        return NFAFragment();
                    }
                    if (end_is_shorthand) {
                        err_code = set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, "Invalid character range with shorthand");
                        return NFAFragment();
                    }
                    if (prev_char > end_char) {
                        err_code = set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, "Invalid character range: start > end");
                        return NFAFragment();
                    }
                    for (unsigned int c = prev_char; c <= end_char; ++c) {
                        new_state->set_char_bit(static_cast<unsigned char>(c));
                    }
                    has_prev_char = false;
                    continue;
                }
            }

            unsigned char atom_char = 0;
            bool is_shorthand = false;
            char shorthand_type = '\0';
            if (!parse_bracket_atom(cursor, atom_char, is_shorthand, shorthand_type, err_buf, err_buf_len, err_code)) {
                return NFAFragment();
            }

            if (is_shorthand) {
                add_char_class_shorthand(new_state, shorthand_type);
                has_prev_char = false;
            } else {
                new_state->set_char_bit(atom_char);
                prev_char = atom_char;
                has_prev_char = true;
            }
        }

        if (cursor.empty() || cursor.front() != ']') {
            err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_BRACKET, "Unclosed bracket expression");
            return NFAFragment();
        }
        cursor.remove_prefix(1); /* consume ']' */
        break;
    }
    case '(': {
        int current_group_id = cursor.next_capture_group_id++;
        NFAState *capture_start_state = new_state;
        capture_start_state->character_code = OPCODE_SPLIT;
        capture_start_state->capture_group_start = current_group_id;

        if (cursor.empty()) {
            err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_GROUP, "Expected pattern inside group");
            return NFAFragment();
        }

        /* Check for empty group () */
        if (cursor.front() == ')') {
            cursor.remove_prefix(1);
            NFAState *capture_end_state = cursor.alloc_state();
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
            err_code = set_error(err_buf, err_buf_len, GREP_ERR_UNCLOSED_GROUP, "Expected ')' to close group");
            return NFAFragment();
        }
        cursor.remove_prefix(1); /* consume ')' */

        NFAState *capture_end_state = cursor.alloc_state();
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
        if (q == '*' || q == '+' || q == '?') {
            if (q == '*') {
                NFAState *split_state = cursor.alloc_state();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;
                for (size_t d = 0; d < current_fragment.dangling_outputs.size(); ++d) {
                    *current_fragment.dangling_outputs[d] = split_state;
                }
                current_fragment.start_state = split_state;
                current_fragment.dangling_outputs.clear();
                current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
            } else if (q == '+') {
                NFAState *split_state = cursor.alloc_state();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;
                for (size_t d = 0; d < current_fragment.dangling_outputs.size(); ++d) {
                    *current_fragment.dangling_outputs[d] = split_state;
                }
                current_fragment.dangling_outputs.clear();
                current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
            } else { /* '?' */
                NFAState *split_state = cursor.alloc_state();
                split_state->character_code = OPCODE_SPLIT;
                split_state->primary_transition = current_fragment.start_state;
                current_fragment.start_state = split_state;
                current_fragment.dangling_outputs.push_back(&split_state->alternative_transition);
            }
            cursor.remove_prefix(1);

            if (!cursor.empty() && (cursor.front() == '*' || cursor.front() == '+' || cursor.front() == '?')) {
                err_code = set_error(err_buf, err_buf_len, GREP_ERR_INVALID_QUANTIFIER, "Consecutive quantifiers are not supported");
                return NFAFragment();
            }
        }
    }

    return current_fragment;
}

static inline int get_precedence(char op) {
    if (op == '|') return 0;
    if (op == ')') return -1;
    return 1;
}

static NFAFragment parse_regex_internal(PatternCursor &cursor, NFAFragment left_fragment, int min_precedence, char *err_buf, size_t err_buf_len, int &err_code) {
    char lookahead = !cursor.empty() ? cursor.front() : '\0';

    while (!cursor.empty() && get_precedence(lookahead) >= min_precedence) {
        char current_op = lookahead;
        if (current_op == '|') {
            cursor.remove_prefix(1);
            if (cursor.empty() || cursor.front() == ')') {
                err_code = set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, "Empty alternation branch");
                return left_fragment;
            }
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
            NFAState *split_state = cursor.alloc_state();
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
    std::vector<NFAState *> arena;
    NFAState *start_state;
    NFAState *matched_state;
    bool is_anchored_start;
    bool matches_empty_string;
    int max_capture_group;

    grep_regex()
        : start_state(NULL),
          matched_state(NULL),
          is_anchored_start(false),
          matches_empty_string(false),
          max_capture_group(0) {}

    ~grep_regex() {
        for (size_t i = 0; i < arena.size(); ++i) {
            delete arena[i];
        }
        arena.clear();
    }

    NFAState *alloc_state() {
        arena.push_back(NULL);
        NFAState *s = new NFAState();
        s->id = static_cast<int>(arena.size()) - 1;
        arena.back() = s;
        return s;
    }
};

/* =========================================================================
 * C API Compilation Implementation
 * ========================================================================= */

int grep_compile_len(const char *pattern, size_t pattern_len, grep_regex_t **out_regex, char *err_buf, size_t err_buf_len) {
    if (!out_regex) {
        return set_error(err_buf, err_buf_len, GREP_ERR_NULL_ARG, "Output regex pointer is NULL");
    }
    *out_regex = NULL;

    if (!pattern && pattern_len > 0) {
        return set_error(err_buf, err_buf_len, GREP_ERR_NULL_ARG, "Pattern pointer is NULL");
    }

    if (err_buf && err_buf_len > 0) {
        err_buf[0] = '\0';
    }

    grep_regex_t *re = NULL;
    try {
        re = new grep_regex_t();
        NFAState *matched_state = re->alloc_state();
        matched_state->character_code = OPCODE_MATCHED;

        re->matched_state = matched_state;
        re->start_state = matched_state;
        re->is_anchored_start = false;
        re->matches_empty_string = (pattern_len == 0);
        re->max_capture_group = 0;

        if (pattern_len == 0) {
            *out_regex = re;
            return GREP_OK;
        }

        PatternCursor cursor(pattern, pattern_len, &re->arena);
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
                return set_error(err_buf, err_buf_len, GREP_ERR_UNMATCHED_PAREN, "Unmatched ')' in pattern");
            }
            return set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, "Syntax error in regular expression");
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
        delete re;
        return set_error(err_buf, err_buf_len, GREP_ERR_NOMEM, "Out of memory");
    } catch (...) {
        delete re;
        return set_error(err_buf, err_buf_len, GREP_ERR_SYNTAX, "Unknown error during regex compilation");
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
    const NFAState *nfa_state;
    ActiveCaptureInfo capture_info;

    ActiveNFAState() : nfa_state(NULL) {}
    ActiveNFAState(const NFAState *s, const ActiveCaptureInfo &c)
        : nfa_state(s), capture_info(c) {}

    bool operator<(const ActiveNFAState &other) const {
        if (nfa_state != other.nfa_state)
            return nfa_state < other.nfa_state;
        return capture_info < other.capture_info;
    }
};

typedef std::vector<ActiveNFAState> ActiveStateList;

struct SimulationContext {
    std::vector<int> visited_marks;
    int current_mark;
    bool has_captures;
    std::vector<int> added_marks;
    std::set<ActiveNFAState> seen_states;

    SimulationContext(size_t num_states, bool captures)
        : visited_marks(num_states, 0),
          current_mark(0),
          has_captures(captures),
          added_marks(num_states, 0) {}

    int next_mark() {
        ++current_mark;
        if (current_mark <= 0) {
            std::fill(visited_marks.begin(), visited_marks.end(), 0);
            std::fill(added_marks.begin(), added_marks.end(), 0);
            current_mark = 1;
        }
        if (has_captures) {
            seen_states.clear();
        }
        return current_mark;
    }
};

static void add_state_with_epsilon_closure(
    const NFAState *state_to_add,
    ActiveCaptureInfo capture_info,
    size_t current_pos,
    bool at_end,
    ActiveStateList &active_states,
    SimulationContext &ctx,
    int closure_mark)
{
    if (!state_to_add) return;
    int sid = state_to_add->id;
    if (ctx.visited_marks[sid] == closure_mark) return;
    ctx.visited_marks[sid] = closure_mark;

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
        add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, current_pos, at_end, active_states, ctx, closure_mark);
        add_state_with_epsilon_closure(state_to_add->alternative_transition, capture_info, current_pos, at_end, active_states, ctx, closure_mark);
        return;
    }

    if (state_to_add->character_code == OPCODE_MATCH_START) {
        if (current_pos == 0) {
            add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, current_pos, at_end, active_states, ctx, closure_mark);
        }
        return;
    }

    if (state_to_add->character_code == OPCODE_MATCH_END && at_end) {
        add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, current_pos, at_end, active_states, ctx, closure_mark);
        return;
    }

    /* Empty capture group backreference matches zero characters (epsilon transition) */
    if (state_to_add->character_code >= OPCODE_BACKREF_START &&
        state_to_add->character_code < OPCODE_BACKREF_START + GREP_MAX_CAPTURE_GROUPS) {
        int gid = state_to_add->character_code - OPCODE_BACKREF_START;
        if (capture_info.captured_spans[gid].valid &&
            !capture_info.is_actively_capturing[gid] &&
            capture_info.captured_spans[gid].start_pos == capture_info.captured_spans[gid].end_pos) {
            add_state_with_epsilon_closure(state_to_add->primary_transition, capture_info, current_pos, at_end, active_states, ctx, closure_mark);
            return;
        }
    }

    if (!ctx.has_captures) {
        if (ctx.added_marks[sid] == ctx.current_mark) return;
        ctx.added_marks[sid] = ctx.current_mark;
        active_states.push_back(ActiveNFAState(state_to_add, capture_info));
    } else {
        ActiveNFAState candidate(state_to_add, capture_info);
        if (ctx.seen_states.insert(candidate).second) {
            active_states.push_back(candidate);
        }
    }
}

static void initialize_active_states(const NFAState *start_state, size_t pos, bool at_end, ActiveStateList &active_states, SimulationContext &ctx) {
    active_states.clear();
    int mark = ctx.next_mark();
    ActiveCaptureInfo initial_capture;
    add_state_with_epsilon_closure(start_state, initial_capture, pos, at_end, active_states, ctx, mark);
}

static bool check_has_matching_state(const ActiveStateList &states, ActiveCaptureInfo *out_caps) {
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
    bool at_end_after_step,
    const char *full_text,
    ActiveStateList &next_states,
    SimulationContext &ctx,
    grep_profiler_t *profiler)
{
    next_states.clear();
    int step_mark = ctx.next_mark();

    if (profiler) {
        profiler->total_steps++;
    }

    unsigned char uc = static_cast<unsigned char>(input_char);

    for (size_t s = 0; s < current_states.size(); ++s) {
        const ActiveNFAState &cur = current_states[s];
        const NFAState *nfa_state = cur.nfa_state;
        ActiveCaptureInfo capture_info = cur.capture_info;

        bool can_consume = false;
        bool character_added_by_backref = false;

        switch (nfa_state->character_code) {
        case OPCODE_MATCH_ANY:
            can_consume = true;
            break;
        case OPCODE_MATCH_DIGIT:
            can_consume = (isdigit(uc) != 0);
            break;
        case OPCODE_MATCH_WORD:
            can_consume = (isalnum(uc) != 0 || uc == '_');
            break;
        case OPCODE_MATCH_SPACE:
            can_consume = (isspace(uc) != 0);
            break;
        case OPCODE_MATCH_CHOICE:
            can_consume = nfa_state->has_char_bit(uc);
            break;
        case OPCODE_MATCH_ANTI_CHOICE:
            can_consume = !nfa_state->has_char_bit(uc);
            break;
        case OPCODE_MATCH_END:
            /* Can only transition at end of input, not by consuming a character */
            break;
        default:
            if (nfa_state->character_code >= OPCODE_BACKREF_START && nfa_state->character_code < OPCODE_BACKREF_START + GREP_MAX_CAPTURE_GROUPS) {
                int gid = nfa_state->character_code - OPCODE_BACKREF_START;
                if (capture_info.captured_spans[gid].valid && !capture_info.is_actively_capturing[gid]) {
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
                                ActiveNFAState mid_state(nfa_state, capture_info);
                                if (!ctx.has_captures || ctx.seen_states.insert(mid_state).second) {
                                    next_states.push_back(mid_state);
                                }
                                continue;
                            }
                        }
                    }
                }
            } else if (nfa_state->character_code == uc) {
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

            /* Each state branch in capture-aware simulation needs its own epsilon closure visit */
            int closure_mark = ctx.has_captures ? ++ctx.current_mark : step_mark;
            if (closure_mark <= 0) {
                std::fill(ctx.visited_marks.begin(), ctx.visited_marks.end(), 0);
                ctx.current_mark = 1;
                closure_mark = 1;
            }
            add_state_with_epsilon_closure(nfa_state->primary_transition, capture_info, next_char_pos, at_end_after_step, next_states, ctx, closure_mark);
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

static int grep_search_internal(
    const grep_regex_t *regex,
    const char *text,
    size_t text_len,
    bool match_only,
    int *out_found,
    grep_match_result_t *out_result,
    grep_profiler_t *profiler)
{
    if (!regex) return GREP_ERR_NULL_ARG;
    if (!text && text_len > 0) return GREP_ERR_NULL_ARG;

    if (out_found) *out_found = 0;
    if (out_result) {
        out_result->found = 0;
        out_result->matches = NULL;
        out_result->match_count = 0;
        out_result->match_capacity = 0;
    }

    if (profiler) {
        profiler->lines_processed++;
    }

    /* Handle empty regex */
    if (regex->matches_empty_string) {
        if (out_found) *out_found = 1;
        if (out_result) {
            out_result->found = 1;
            out_result->matches = static_cast<grep_single_match_t *>(calloc(1, sizeof(grep_single_match_t)));
            if (!out_result->matches) return GREP_ERR_NOMEM;
            out_result->match_count = 1;
            out_result->match_capacity = 1;
        }
        return GREP_OK;
    }

    std::vector<grep_single_match_t> collected_matches;
    SimulationContext ctx(regex->arena.size(), regex->max_capture_group > 0);
    ActiveStateList current_states, next_states;

    size_t current_global_pos = 0;
    bool is_anchored_start = regex->is_anchored_start;

    while (current_global_pos <= text_len) {
        size_t remaining_len = text_len - current_global_pos;
        const char *remaining_text = (text_len > 0 && current_global_pos < text_len) ? (text + current_global_pos) : "";

        if (is_anchored_start && current_global_pos > 0) {
            break;
        }

        initialize_active_states(regex->start_state, current_global_pos, remaining_len == 0, current_states, ctx);

        bool match_found_in_segment = false;
        size_t match_length = 0;
        ActiveCaptureInfo winning_caps;

        for (size_t i = 0; i <= remaining_len; ++i) {
            if (check_has_matching_state(current_states, &winning_caps)) {
                match_found_in_segment = true;
                match_length = i;
                if (match_only) {
                    if (out_found) *out_found = 1;
                    return GREP_OK;
                }
            }

            if (i == remaining_len || current_states.empty()) {
                break;
            }

            char input_char = remaining_text[i];
            bool at_end_after_step = (i + 1 == remaining_len);
            process_character_step(current_states, input_char, current_global_pos + i + 1, at_end_after_step, text, next_states, ctx, profiler);
            current_states.swap(next_states);

            if (current_states.empty()) {
                break;
            }
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
        if (out_found) *out_found = 1;
        if (out_result) {
            out_result->found = 1;
            out_result->match_count = collected_matches.size();
            out_result->match_capacity = collected_matches.size();
            out_result->matches = static_cast<grep_single_match_t *>(malloc(sizeof(grep_single_match_t) * collected_matches.size()));
            if (!out_result->matches) return GREP_ERR_NOMEM;
            memcpy(out_result->matches, &collected_matches[0], sizeof(grep_single_match_t) * collected_matches.size());
        }
    }

    return GREP_OK;
}

int grep_find_all_len(const grep_regex_t *regex, const char *text, size_t text_len, grep_match_result_t *out_result, grep_profiler_t *profiler) {
    if (!out_result) return GREP_ERR_NULL_ARG;
    return grep_search_internal(regex, text, text_len, false, NULL, out_result, profiler);
}

int grep_find_all(const grep_regex_t *regex, const char *text, grep_match_result_t *out_result, grep_profiler_t *profiler) {
    if (!text) return grep_find_all_len(regex, NULL, 0, out_result, profiler);
    return grep_find_all_len(regex, text, strlen(text), out_result, profiler);
}

int grep_match_len(const grep_regex_t *regex, const char *text, size_t text_len, grep_profiler_t *profiler) {
    int found = 0;
    int rc = grep_search_internal(regex, text, text_len, true, &found, NULL, profiler);
    if (rc != GREP_OK) return rc;
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
