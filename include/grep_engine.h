#ifndef GREP_ENGINE_H
#define GREP_ENGINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Error codes returned by the C API.
 */
typedef enum grep_error_code {
    GREP_OK = 0,
    GREP_ERR_NULL_ARG = -1,
    GREP_ERR_NOMEM = -2,
    GREP_ERR_SYNTAX = -3,
    GREP_ERR_UNEXPECTED_END = -4,
    GREP_ERR_UNCLOSED_BRACKET = -5,
    GREP_ERR_UNCLOSED_GROUP = -6,
    GREP_ERR_UNMATCHED_PAREN = -7,
    GREP_ERR_UNMATCHED_BRACKET = -8,
    GREP_ERR_INVALID_QUANTIFIER = -9,
    GREP_ERR_IO = -10
} grep_error_code_t;

/**
 * Maximum number of capture groups supported (groups 1..9).
 */
#define GREP_MAX_CAPTURE_GROUPS 10

/**
 * Represents a half-open byte range [start_pos, end_pos) within matched text.
 */
typedef struct grep_span {
    size_t start_pos;
    size_t end_pos;
    int valid; /* 1 if matched/captured, 0 otherwise */
} grep_span_t;

/**
 * Represents a single match occurrence and its capture groups.
 */
typedef struct grep_single_match {
    size_t start_pos;
    size_t end_pos;
    grep_span_t captures[GREP_MAX_CAPTURE_GROUPS]; /* index 1..9 for \1..\9 */
} grep_single_match_t;

/**
 * Profiler statistics collected during NFA simulation.
 */
typedef struct grep_profiler {
    size_t total_steps;
    size_t total_states_visited;
    size_t max_active_states;
    size_t lines_processed;
} grep_profiler_t;

/**
 * Opaque handle to a compiled NFA regular expression.
 */
typedef struct grep_regex grep_regex_t;

/**
 * Result of matching against a text string (holds all non-overlapping matches).
 */
typedef struct grep_match_result {
    int found;                      /* 1 if at least one match was found, 0 otherwise */
    grep_single_match_t *matches;   /* Array of matches (caller frees via grep_match_result_free) */
    size_t match_count;             /* Number of elements in matches */
    size_t match_capacity;          /* Allocated capacity of matches */
} grep_match_result_t;

/**
 * Compile a regular expression string into an NFA.
 *
 * @param pattern     Null-terminated regex pattern string.
 * @param out_regex   Output pointer to receive the compiled regex handle.
 * @param err_buf     Optional buffer to receive a human-readable error message on failure.
 * @param err_buf_len Size of err_buf in bytes.
 * @return GREP_OK (0) on success, or a negative grep_error_code_t on failure.
 */
int grep_compile(const char *pattern, grep_regex_t **out_regex, char *err_buf, size_t err_buf_len);

/**
 * Compile a regular expression of explicit length into an NFA.
 */
int grep_compile_len(const char *pattern, size_t pattern_len, grep_regex_t **out_regex, char *err_buf, size_t err_buf_len);

/**
 * Free a compiled regex handle. Safe to call with NULL.
 */
void grep_free(grep_regex_t *regex);

/**
 * Test whether the given null-terminated text matches the compiled regex anywhere.
 *
 * @param regex Compiled regex handle.
 * @param text  Null-terminated input text.
 * @return 1 if matched, 0 if not matched, or negative grep_error_code_t on invalid arguments.
 */
int grep_match(const grep_regex_t *regex, const char *text);

/**
 * Test whether the given text of explicit length matches the compiled regex anywhere.
 */
int grep_match_len(const grep_regex_t *regex, const char *text, size_t text_len, grep_profiler_t *profiler);

/**
 * Find all non-overlapping matches of the compiled regex in the null-terminated text.
 *
 * @param regex      Compiled regex handle.
 * @param text       Null-terminated input text.
 * @param out_result Output structure to populate (must be freed with grep_match_result_free).
 * @param profiler   Optional pointer to profiler stats to accumulate into (may be NULL).
 * @return GREP_OK (0) on success, or negative grep_error_code_t on failure.
 */
int grep_find_all(const grep_regex_t *regex, const char *text, grep_match_result_t *out_result, grep_profiler_t *profiler);

/**
 * Find all non-overlapping matches in text of explicit length.
 */
int grep_find_all_len(const grep_regex_t *regex, const char *text, size_t text_len, grep_match_result_t *out_result, grep_profiler_t *profiler);

/**
 * Free dynamic memory inside a grep_match_result_t.
 */
void grep_match_result_free(grep_match_result_t *result);

/**
 * Reset profiler counters to zero.
 */
void grep_profiler_reset(grep_profiler_t *profiler);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* =========================================================================
 * C++ Wrapper API (Compatible with C++98 / C++03 / C++11 / C++17 / C++23)
 * ========================================================================= */
#ifdef __cplusplus

#include <string>
#include <vector>
#include <utility>
#include <stdexcept>

namespace grep {

/**
 * Exception thrown when regex compilation fails in the C++ API.
 */
class RegexError : public std::runtime_error {
public:
    explicit RegexError(const std::string &message, int code = GREP_ERR_SYNTAX)
        : std::runtime_error(message), code_(code) {}

    int code() const throw() { return code_; }

private:
    int code_;
};

/**
 * Represents a single match range and its captured groups.
 */
struct SingleMatch {
    size_t start_pos;
    size_t end_pos;
    std::vector<std::pair<bool, std::string> > captures;

    SingleMatch() : start_pos(0), end_pos(0) {}
    SingleMatch(size_t s, size_t e) : start_pos(s), end_pos(e) {}
};

/**
 * Holds the result of matching a pattern against a line/text.
 */
struct MatchInfo {
    bool found;
    std::vector<std::pair<size_t, size_t> > matches;
    std::vector<SingleMatch> detailed_matches;

    MatchInfo() : found(false) {}
};

/**
 * Profiling counters for NFA execution.
 */
struct Profiler {
    grep_profiler_t raw;

    Profiler() { reset(); }

    void reset() {
        grep_profiler_reset(&raw);
    }

    size_t total_steps() const { return raw.total_steps; }
    size_t total_states_visited() const { return raw.total_states_visited; }
    size_t max_active_states() const { return raw.max_active_states; }
    size_t lines_processed() const { return raw.lines_processed; }
};

/**
 * RAII C++ class encapsulating a compiled NFA regular expression.
 * Works across C++98 through C++23.
 */
class Regex {
public:
    explicit Regex(const std::string &pattern) : handle_(NULL) {
        compile_internal(pattern.data(), pattern.size());
    }

    explicit Regex(const char *pattern) : handle_(NULL) {
        if (!pattern) {
            throw RegexError("Null regex pattern", GREP_ERR_NULL_ARG);
        }
        size_t len = 0;
        while (pattern[len] != '\0') {
            ++len;
        }
        compile_internal(pattern, len);
    }

    Regex(const Regex &other) : pattern_(other.pattern_), handle_(NULL) {
        if (other.handle_) {
            compile_internal(pattern_.data(), pattern_.size());
        }
    }

    Regex &operator=(const Regex &other) {
        if (this != &other) {
            grep_regex_t *new_handle = NULL;
            if (other.handle_) {
                char err[256];
                int rc = grep_compile_len(other.pattern_.data(), other.pattern_.size(), &new_handle, err, sizeof(err));
                if (rc != GREP_OK) {
                    throw RegexError(err, rc);
                }
            }
            if (handle_) {
                grep_free(handle_);
            }
            handle_ = new_handle;
            pattern_ = other.pattern_;
        }
        return *this;
    }

#if __cplusplus >= 201103L
    Regex(Regex &&other) noexcept : pattern_(std::move(other.pattern_)), handle_(other.handle_) {
        other.handle_ = NULL;
    }

    Regex &operator=(Regex &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                grep_free(handle_);
            }
            pattern_ = std::move(other.pattern_);
            handle_ = other.handle_;
            other.handle_ = NULL;
        }
        return *this;
    }
#endif

    ~Regex() {
        if (handle_) {
            grep_free(handle_);
            handle_ = NULL;
        }
    }

    /**
     * Returns true if the pattern matches anywhere in text.
     */
    bool matches(const std::string &text, Profiler *profiler = NULL) const {
        return grep_match_len(handle_, text.data(), text.size(), profiler ? &profiler->raw : NULL) == 1;
    }

    bool matches(const char *text, Profiler *profiler = NULL) const {
        if (!text) return false;
        size_t len = 0;
        while (text[len] != '\0') ++len;
        return grep_match_len(handle_, text, len, profiler ? &profiler->raw : NULL) == 1;
    }

    /**
     * Finds all non-overlapping matches in text.
     */
    MatchInfo find_all(const std::string &text, Profiler *profiler = NULL) const {
        return find_all_len(text.data(), text.size(), profiler);
    }

    MatchInfo find_all(const char *text, Profiler *profiler = NULL) const {
        if (!text) return MatchInfo();
        size_t len = 0;
        while (text[len] != '\0') ++len;
        return find_all_len(text, len, profiler);
    }

    MatchInfo find_all_len(const char *text, size_t text_len, Profiler *profiler = NULL) const {
        MatchInfo info;
        grep_match_result_t res;
        int rc = grep_find_all_len(handle_, text, text_len, &res, profiler ? &profiler->raw : NULL);
        if (rc != GREP_OK) {
            return info;
        }
        info.found = (res.found != 0);
        if (res.found && res.match_count > 0 && res.matches != NULL) {
            info.matches.reserve(res.match_count);
            info.detailed_matches.reserve(res.match_count);
            for (size_t i = 0; i < res.match_count; ++i) {
                const grep_single_match_t &m = res.matches[i];
                info.matches.push_back(std::make_pair(m.start_pos, m.end_pos));
                SingleMatch sm(m.start_pos, m.end_pos);
                sm.captures.resize(GREP_MAX_CAPTURE_GROUPS);
                for (int cg = 0; cg < GREP_MAX_CAPTURE_GROUPS; ++cg) {
                    if (m.captures[cg].valid) {
                        size_t c_start = m.captures[cg].start_pos;
                        size_t c_end = m.captures[cg].end_pos;
                        if (c_end >= c_start && c_end <= text_len) {
                            sm.captures[cg] = std::make_pair(true, std::string(text + c_start, c_end - c_start));
                        } else {
                            sm.captures[cg] = std::make_pair(false, std::string());
                        }
                    } else {
                        sm.captures[cg] = std::make_pair(false, std::string());
                    }
                }
                info.detailed_matches.push_back(sm);
            }
        }
        grep_match_result_free(&res);
        return info;
    }

    const std::string &pattern() const { return pattern_; }
    const grep_regex_t *c_handle() const { return handle_; }

private:
    std::string pattern_;
    grep_regex_t *handle_;

    void compile_internal(const char *pat, size_t len) {
        pattern_.assign(pat, len);
        char err[256];
        int rc = grep_compile_len(pat, len, &handle_, err, sizeof(err));
        if (rc != GREP_OK) {
            throw RegexError(err, rc);
        }
    }
};

/**
 * Convenience function to format a matched line with ANSI color codes.
 */
inline std::string format_colored_line(const std::string &line, const MatchInfo &match_info, bool use_color = true) {
    if (!use_color || !match_info.found || match_info.matches.empty()) {
        return line;
    }
    const char *color_red_bold = "\033[1;31m";
    const char *color_reset = "\033[0m";
    std::string out;
    size_t current_pos = 0;
    for (size_t i = 0; i < match_info.matches.size(); ++i) {
        size_t match_start = match_info.matches[i].first;
        size_t match_end = match_info.matches[i].second;
        if (match_start > current_pos) {
            out.append(line, current_pos, match_start - current_pos);
        }
        if (match_end > match_start) {
            out.append(color_red_bold);
            out.append(line, match_start, match_end - match_start);
            out.append(color_reset);
        }
        current_pos = match_end;
    }
    if (current_pos < line.size()) {
        out.append(line, current_pos, line.size() - current_pos);
    }
    return out;
}

} /* namespace grep */

#endif /* __cplusplus */

#endif /* GREP_ENGINE_H */
