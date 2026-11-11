#include "grep_engine.h"
#include <iostream>
#include <cassert>
#include <string>

static int total_cpp_tests = 0;
static int passed_cpp_tests = 0;

#define CPP_ASSERT_TRUE(expr) do { \
    total_cpp_tests++; \
    if (expr) { \
        passed_cpp_tests++; \
    } else { \
        std::cerr << "FAILED [Line " << __LINE__ << "]: " << #expr << std::endl; \
    } \
} while (0)

#define CPP_ASSERT_EQ(a, b) do { \
    total_cpp_tests++; \
    if ((a) == (b)) { \
        passed_cpp_tests++; \
    } else { \
        std::cerr << "FAILED [Line " << __LINE__ << "]: " << (a) << " != " << (b) << std::endl; \
    } \
} while (0)

static void test_literals_and_wildcard() {
    grep::Regex re("cat.");
    CPP_ASSERT_TRUE(re.matches("cats"));
    CPP_ASSERT_TRUE(re.matches("cate"));
    CPP_ASSERT_TRUE(re.matches("dog and cats"));
    CPP_ASSERT_TRUE(!re.matches("cat"));
}

static void test_character_classes() {
    grep::Regex re("[abc]");
    CPP_ASSERT_TRUE(re.matches("apple"));
    CPP_ASSERT_TRUE(re.matches("banana"));
    CPP_ASSERT_TRUE(!re.matches("dog"));

    grep::Regex re_neg("[^abc]");
    CPP_ASSERT_TRUE(re_neg.matches("d"));
    CPP_ASSERT_TRUE(re_neg.matches("apple")); /* 'p', 'l', 'e' match negation */
    CPP_ASSERT_TRUE(!re_neg.matches("a"));

    grep::Regex re_range("^[a-z]+$");
    CPP_ASSERT_TRUE(re_range.matches("apple"));
    CPP_ASSERT_TRUE(re_range.matches("orange"));
    CPP_ASSERT_TRUE(!re_range.matches("Apple"));
    CPP_ASSERT_TRUE(!re_range.matches("123"));

    grep::Regex re_edge_bracket("[]a-]");
    CPP_ASSERT_TRUE(re_edge_bracket.matches("]"));
    CPP_ASSERT_TRUE(re_edge_bracket.matches("-"));
    CPP_ASSERT_TRUE(re_edge_bracket.matches("a"));
    CPP_ASSERT_TRUE(!re_edge_bracket.matches("b"));

    grep::Regex re_digits("\\d+");
    CPP_ASSERT_TRUE(re_digits.matches("order 12345 completed"));
    CPP_ASSERT_TRUE(!re_digits.matches("order completed"));

    grep::Regex re_words("\\w+");
    CPP_ASSERT_TRUE(re_words.matches("hello_world"));
}

static void test_quantifiers() {
    grep::Regex re_star("ab*c");
    CPP_ASSERT_TRUE(re_star.matches("ac"));
    CPP_ASSERT_TRUE(re_star.matches("abc"));
    CPP_ASSERT_TRUE(re_star.matches("abbbbc"));
    CPP_ASSERT_TRUE(!re_star.matches("ab"));

    grep::Regex re_plus("ab+c");
    CPP_ASSERT_TRUE(!re_plus.matches("ac"));
    CPP_ASSERT_TRUE(re_plus.matches("abc"));
    CPP_ASSERT_TRUE(re_plus.matches("abbbc"));

    grep::Regex re_question("ab?c");
    CPP_ASSERT_TRUE(re_question.matches("ac"));
    CPP_ASSERT_TRUE(re_question.matches("abc"));
    CPP_ASSERT_TRUE(!re_question.matches("abbc"));

    /* Greedy longest match verification */
    grep::Regex re_greedy("a.*a");
    grep::MatchInfo mi = re_greedy.find_all("xababax");
    CPP_ASSERT_TRUE(mi.found);
    CPP_ASSERT_EQ(mi.matches.size(), 1u);
    CPP_ASSERT_EQ(mi.matches[0].first, 1u);
    CPP_ASSERT_EQ(mi.matches[0].second, 6u);
}

static void test_alternation() {
    grep::Regex re("cat|dog");
    CPP_ASSERT_TRUE(re.matches("my cat"));
    CPP_ASSERT_TRUE(re.matches("my dog"));
    CPP_ASSERT_TRUE(!re.matches("my fish"));

    grep::Regex re_complex("I like (cats|dogs)!");
    CPP_ASSERT_TRUE(re_complex.matches("I like cats!"));
    CPP_ASSERT_TRUE(re_complex.matches("I like dogs!"));
    CPP_ASSERT_TRUE(!re_complex.matches("I like birds!"));
}

static void test_groups_and_backreferences() {
    /* \1 matches first capture group */
    grep::Regex re("(\\w+)\\s+\\1");
    CPP_ASSERT_TRUE(re.matches("hello hello world"));
    CPP_ASSERT_TRUE(re.matches("test test"));
    CPP_ASSERT_TRUE(!re.matches("hello world"));

    grep::MatchInfo mi = re.find_all("apple apple and banana banana");
    CPP_ASSERT_TRUE(mi.found);
    CPP_ASSERT_EQ(mi.matches.size(), 2u);
    CPP_ASSERT_EQ(mi.detailed_matches.size(), 2u);

    /* Check capture group text */
    CPP_ASSERT_TRUE(mi.detailed_matches[0].captures[1].first);
    CPP_ASSERT_EQ(mi.detailed_matches[0].captures[1].second, "apple");

    CPP_ASSERT_TRUE(mi.detailed_matches[1].captures[1].first);
    CPP_ASSERT_EQ(mi.detailed_matches[1].captures[1].second, "banana");

    /* Empty capture group backreference */
    grep::Regex re_empty("^(a?)\\1$");
    CPP_ASSERT_TRUE(re_empty.matches(""));
    CPP_ASSERT_TRUE(re_empty.matches("aa"));
    CPP_ASSERT_TRUE(!re_empty.matches("a"));
}

static void test_anchors() {
    grep::Regex start_anchor("^Hello");
    CPP_ASSERT_TRUE(start_anchor.matches("Hello world"));
    CPP_ASSERT_TRUE(!start_anchor.matches("world Hello"));

    grep::Regex end_anchor("world$");
    CPP_ASSERT_TRUE(end_anchor.matches("Hello world"));
    CPP_ASSERT_TRUE(!end_anchor.matches("Hello world!"));

    grep::Regex exact("^exact$");
    CPP_ASSERT_TRUE(exact.matches("exact"));
    CPP_ASSERT_TRUE(!exact.matches("not exact"));
    CPP_ASSERT_TRUE(!exact.matches("exact not"));

    grep::Regex grouped_anchor("(^foo$)|(^bar$)");
    CPP_ASSERT_TRUE(grouped_anchor.matches("foo"));
    CPP_ASSERT_TRUE(grouped_anchor.matches("bar"));
    CPP_ASSERT_TRUE(!grouped_anchor.matches("xfoo"));
    CPP_ASSERT_TRUE(!grouped_anchor.matches("barx"));

    grep::Regex empty_line("^$");
    CPP_ASSERT_TRUE(empty_line.matches(""));
    CPP_ASSERT_TRUE(!empty_line.matches("a"));
}

static void test_copy_and_assign() {
    grep::Regex r1("([0-9]+)");
    grep::Regex r2(r1);
    grep::Regex r3("abc");
    r3 = r1;

    CPP_ASSERT_TRUE(r2.matches("val=42"));
    CPP_ASSERT_TRUE(r3.matches("val=99"));
    CPP_ASSERT_EQ(r3.pattern(), "([0-9]+)");
}

static void test_syntax_errors() {
    bool caught = false;
    try {
        grep::Regex invalid("[unclosed");
    } catch (const grep::RegexError &) {
        caught = true;
    }
    CPP_ASSERT_TRUE(caught);

    caught = false;
    try {
        grep::Regex invalid("(unclosed group");
    } catch (const grep::RegexError &) {
        caught = true;
    }
    CPP_ASSERT_TRUE(caught);

    caught = false;
    try {
        grep::Regex invalid("unmatched)");
    } catch (const grep::RegexError &) {
        caught = true;
    }
    CPP_ASSERT_TRUE(caught);

    caught = false;
    try {
        grep::Regex invalid(")");
    } catch (const grep::RegexError &) {
        caught = true;
    }
    CPP_ASSERT_TRUE(caught);

    caught = false;
    try {
        grep::Regex invalid("a**");
    } catch (const grep::RegexError &) {
        caught = true;
    }
    CPP_ASSERT_TRUE(caught);

    caught = false;
    try {
        grep::Regex invalid("a|");
    } catch (const grep::RegexError &) {
        caught = true;
    }
    CPP_ASSERT_TRUE(caught);
}

static void test_profiler() {
    grep::Profiler profiler;
    grep::Regex re("a*b");
    re.matches("aaaaaaaaab", &profiler);

    CPP_ASSERT_TRUE(profiler.lines_processed() >= 1);
    CPP_ASSERT_TRUE(profiler.total_steps() > 0);
    CPP_ASSERT_TRUE(profiler.total_states_visited() > 0);
    CPP_ASSERT_TRUE(profiler.max_active_states() > 0);

    profiler.reset();
    CPP_ASSERT_EQ(profiler.total_steps(), 0u);
}

static void test_colored_formatting() {
    grep::Regex re("test");
    std::string text = "This is a test line for test.";
    grep::MatchInfo mi = re.find_all(text);
    CPP_ASSERT_TRUE(mi.found);
    CPP_ASSERT_EQ(mi.matches.size(), 2u);

    std::string colored = grep::format_colored_line(text, mi, true);
    CPP_ASSERT_TRUE(colored.find("\033[1;31mtest\033[0m") != std::string::npos);

    std::string uncolored = grep::format_colored_line(text, mi, false);
    CPP_ASSERT_EQ(uncolored, text);
}

int main() {
    std::cout << "--- Running C++ API Tests ---" << std::endl;
    test_literals_and_wildcard();
    test_character_classes();
    test_quantifiers();
    test_alternation();
    test_groups_and_backreferences();
    test_anchors();
    test_copy_and_assign();
    test_syntax_errors();
    test_profiler();
    test_colored_formatting();

    std::cout << "C++ Tests Summary: " << passed_cpp_tests << " / " << total_cpp_tests << " passed" << std::endl;
    return (passed_cpp_tests == total_cpp_tests) ? 0 : 1;
}
