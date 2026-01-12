#include "test_common.h"

int test_parser_yield_rules(void) {
  // yield outside async => error
  {
    const char* src = "fn main() -> int { yield\nreturn 0 }\n";
    int errors = ai_test_parse_errors(src);
    AI_TEST_ASSERT(errors > 0);
  }

  // yield inside async => ok
  {
    const char* src = "async fn f() -> int { yield\nreturn 0 }\n";
    int errors = ai_test_parse_errors(src);
    AI_TEST_ASSERT(errors == 0);
  }

  return 0;
}

int test_parser_await_rules(void) {
  // await outside async => error
  {
    const char* src = "fn main() -> int { let x = await f(); return 0 }\n";
    int errors = ai_test_parse_errors(src);
    AI_TEST_ASSERT(errors > 0);
  }

  // await inside async => ok (syntax only; f() is unresolved but allowed at this stage)
  {
    const char* src = "async fn main() -> int { let x = await f(); return x }\n";
    int errors = ai_test_parse_errors(src);
    AI_TEST_ASSERT(errors == 0);
  }

  return 0;
}

int test_parser_top_level_let_forbidden(void) {
  const char* src = "let x: int = 1\nfn main() -> int { return 0 }\n";
  int errors = ai_test_parse_errors(src);
  AI_TEST_ASSERT(errors > 0);
  return 0;
}

