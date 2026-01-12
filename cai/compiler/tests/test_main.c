#include "test_common.h"

// Forward declarations (implemented in other test translation units).
int test_lexer_keywords(void);
int test_parser_yield_rules(void);
int test_parser_await_rules(void);
int test_parser_top_level_let_forbidden(void);

static int run_all(const AiTestCase* cases, size_t n) {
  int failures = 0;
  for (size_t i = 0; i < n; i++) {
    const AiTestCase* tc = &cases[i];
    int r = tc->fn();
    if (r != 0) {
      failures += 1;
      fprintf(stderr, "[FAIL] %s\n", tc->name);
    } else {
      fprintf(stderr, "[ OK ] %s\n", tc->name);
    }
  }
  return failures;
}

int main(void) {
  const AiTestCase cases[] = {
      {"lexer: keywords", test_lexer_keywords},
      {"parser: yield rules", test_parser_yield_rules},
      {"parser: await rules", test_parser_await_rules},
      {"parser: top-level let forbidden", test_parser_top_level_let_forbidden},
  };

  int failures = run_all(cases, sizeof(cases) / sizeof(cases[0]));
  if (failures != 0) {
    fprintf(stderr, "\n%d test(s) failed\n", failures);
    return 1;
  }
  fprintf(stderr, "\nall tests passed\n");
  return 0;
}

