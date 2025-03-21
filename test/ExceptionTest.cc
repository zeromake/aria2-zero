#include "Exception.h"

#include <iostream>
#include <cppunit/extensions/HelperMacros.h>

#include "DownloadFailureException.h"
#include "util.h"
#include "A2STR.h"

namespace aria2 {

class ExceptionTest : public CppUnit::TestFixture {

  CPPUNIT_TEST_SUITE(ExceptionTest);
  CPPUNIT_TEST(testStackTrace);
  CPPUNIT_TEST_SUITE_END();

public:
  void setUp() {}

  void tearDown() {}

  void testStackTrace();
};

CPPUNIT_TEST_SUITE_REGISTRATION(ExceptionTest);

void ExceptionTest::testStackTrace()
{
  DownloadFailureException c1 =
      DOWNLOAD_FAILURE_EXCEPTION2("cause1", error_code::TIME_OUT);
  DownloadFailureException c2 = DOWNLOAD_FAILURE_EXCEPTION2("cause2", c1);
  DownloadFailureException e =
      DOWNLOAD_FAILURE_EXCEPTION2("exception thrown", c2);

  std::string expected = "Exception: [test/ExceptionTest.cc:34] errorCode=2 exception thrown\n"
    "  -> [test/ExceptionTest.cc:32] errorCode=2 cause2\n"
    "  -> [test/ExceptionTest.cc:31] errorCode=2 cause1\n";
#ifdef _WIN32
  expected = util::replace(expected, "[test/", "[test\\");
#endif
  CPPUNIT_ASSERT_EQUAL(
      expected,
      util::replace(e.stackTrace(), std::string(A2_TEST_DIR) + "/", ""));
}

} // namespace aria2
