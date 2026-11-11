#include "grep_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total_c_tests = 0;
static int passed_c_tests = 0;

#define ASSERT_TRUE(expr) do { \
    total_c_tests++; \
    if (expr) { \
        passed_c_tests++; \
    } else { \
        printf("FAILED [Line %d]: %s\n", __LINE__, #expr); \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
    total_c_tests++; \
    if ((a) == (b)) { \
        passed_c_tests++; \
    } else { \
        printf("FAILED [Line %d]: %d != %d\n", __LINE__, (int)(a), (int)(b)); \
    } \
} while (0)

static void test_c_compilation(void) {
    grep_regex_t *re = NULL;
    char err[128];

    ASSERT_EQ_INT(grep_compile("abc", &re, err, sizeof(err)), GREP_OK);
    ASSERT_TRUE(re != NULL);
    grep_free(re);
    re = NULL;

    /* Invalid patterns */
    ASSERT_EQ_INT(grep_compile("[a-z", &re, err, sizeof(err)), GREP_ERR_UNCLOSED_BRACKET);
    ASSERT_TRUE(re == NULL);
    ASSERT_TRUE(strlen(err) > 0);

    ASSERT_EQ_INT(grep_compile(")", &re, err, sizeof(err)), GREP_ERR_UNMATCHED_PAREN);
    ASSERT_TRUE(re == NULL);

    ASSERT_EQ_INT(grep_compile("*abc", &re, err, sizeof(err)), GREP_ERR_INVALID_QUANTIFIER);
    ASSERT_TRUE(re == NULL);

    ASSERT_EQ_INT(grep_compile("a**", &re, err, sizeof(err)), GREP_ERR_INVALID_QUANTIFIER);
    ASSERT_TRUE(re == NULL);

    ASSERT_EQ_INT(grep_compile("[z-a]", &re, err, sizeof(err)), GREP_ERR_SYNTAX);
    ASSERT_TRUE(re == NULL);

    /* Null arguments */
    ASSERT_EQ_INT(grep_compile(NULL, &re, err, sizeof(err)), GREP_OK);
    ASSERT_TRUE(re != NULL);
    ASSERT_EQ_INT(grep_match(re, "anything"), 1);
    grep_free(re);
    re = NULL;

    ASSERT_EQ_INT(grep_compile_len(NULL, 5, &re, err, sizeof(err)), GREP_ERR_NULL_ARG);
    ASSERT_TRUE(re == NULL);
}

static void test_c_matching(void) {
    grep_regex_t *re = NULL;
    char err[128];
    ASSERT_EQ_INT(grep_compile("\\d+", &re, err, sizeof(err)), GREP_OK);

    ASSERT_EQ_INT(grep_match(re, "hello 123 world"), 1);
    ASSERT_EQ_INT(grep_match(re, "no digits here"), 0);

    grep_free(re);
    re = NULL;

    /* Character ranges */
    ASSERT_EQ_INT(grep_compile("^[a-z0-9]+$", &re, err, sizeof(err)), GREP_OK);
    ASSERT_EQ_INT(grep_match(re, "abc123"), 1);
    ASSERT_EQ_INT(grep_match(re, "ABC"), 0);
    grep_free(re);
}

static void test_c_find_all_and_captures(void) {
    grep_regex_t *re = NULL;
    char err[128];
    const char *text = "the the quick fox fox jumps";
    grep_match_result_t res;

    /* Repeated word pattern */
    ASSERT_EQ_INT(grep_compile("(\\w+)\\s+\\1", &re, err, sizeof(err)), GREP_OK);

    ASSERT_EQ_INT(grep_find_all(re, text, &res, NULL), GREP_OK);
    ASSERT_EQ_INT(res.found, 1);
    ASSERT_EQ_INT((int)res.match_count, 2);

    /* First match: "the the" */
    ASSERT_EQ_INT((int)res.matches[0].start_pos, 0);
    ASSERT_EQ_INT((int)res.matches[0].end_pos, 7);
    ASSERT_EQ_INT(res.matches[0].captures[1].valid, 1);
    ASSERT_EQ_INT((int)res.matches[0].captures[1].start_pos, 0);
    ASSERT_EQ_INT((int)res.matches[0].captures[1].end_pos, 3);

    grep_match_result_free(&res);
    grep_free(re);
}

static void test_c_greedy_find_all(void) {
    grep_regex_t *re = NULL;
    grep_match_result_t res;

    ASSERT_EQ_INT(grep_compile("\\d+", &re, NULL, 0), GREP_OK);
    ASSERT_EQ_INT(grep_find_all(re, "order 12345 and 678", &res, NULL), GREP_OK);
    ASSERT_EQ_INT(res.found, 1);
    ASSERT_EQ_INT((int)res.match_count, 2);
    ASSERT_EQ_INT((int)res.matches[0].start_pos, 6);
    ASSERT_EQ_INT((int)res.matches[0].end_pos, 11);
    ASSERT_EQ_INT((int)res.matches[1].start_pos, 16);
    ASSERT_EQ_INT((int)res.matches[1].end_pos, 19);

    grep_match_result_free(&res);
    grep_free(re);
}

static void test_c_anchors(void) {
    grep_regex_t *re = NULL;
    ASSERT_EQ_INT(grep_compile("^start", &re, NULL, 0), GREP_OK);
    ASSERT_EQ_INT(grep_match(re, "start of line"), 1);
    ASSERT_EQ_INT(grep_match(re, "not start"), 0);
    grep_free(re);

    ASSERT_EQ_INT(grep_compile("end$", &re, NULL, 0), GREP_OK);
    ASSERT_EQ_INT(grep_match(re, "at the end"), 1);
    ASSERT_EQ_INT(grep_match(re, "end of line not quite"), 0);
    grep_free(re);

    ASSERT_EQ_INT(grep_compile("^a|^b", &re, NULL, 0), GREP_OK);
    ASSERT_EQ_INT(grep_match(re, "apple"), 1);
    ASSERT_EQ_INT(grep_match(re, "banana"), 1);
    ASSERT_EQ_INT(grep_match(re, "xapple"), 0);
    grep_free(re);
}

int main(void) {
    printf("--- Running Pure C API Tests ---\n");
    test_c_compilation();
    test_c_matching();
    test_c_find_all_and_captures();
    test_c_greedy_find_all();
    test_c_anchors();

    printf("C Tests Summary: %d / %d passed\n", passed_c_tests, total_c_tests);
    return (passed_c_tests == total_c_tests) ? 0 : 1;
}
