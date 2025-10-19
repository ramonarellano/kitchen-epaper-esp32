#include <unity.h>
#include "../..//src/uart_helpers.h"

void test_build_header() {
  uint8_t header[4];
  buildImageHeader(0x01020304, header);
  TEST_ASSERT_EQUAL_HEX8(0x01, header[0]);
  TEST_ASSERT_EQUAL_HEX8(0x02, header[1]);
  TEST_ASSERT_EQUAL_HEX8(0x03, header[2]);
  TEST_ASSERT_EQUAL_HEX8(0x04, header[3]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_build_header);
  return UNITY_END();
}
