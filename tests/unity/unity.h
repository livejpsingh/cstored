/* =========================================================================
    Unity Project - A Test Framework for C
    Copyright (c) 2007-21 Mike Karlesky, Mark VanderVoord, Greg Williams
    [Released under MIT License. Please refer to license.txt for details]
========================================================================= */

#ifndef UNITY_FRAMEWORK_H
#define UNITY_FRAMEWORK_H

#define UNITY

#include "unity_internals.h"

#ifdef __cplusplus
extern "C"
{
#endif

void UnityBegin(const char* filename);
int  UnityEnd(void);
void UnityDefaultTestRun(UnityTestFunction Func, const char* FuncName, const int FuncLineNum);

/* Forward declarations of setup / teardown */
void setUp(void);
void tearDown(void);
void suiteSetUp(void);
int  suiteTearDown(int num_failures);
void resetTest(void);

#define RUN_TEST(func) UnityDefaultTestRun(func, #func, __LINE__)

#define TEST_PROTECT() (setjmp(Unity.AbortFrame) == 0)

#define TEST_ABORT() longjmp(Unity.AbortFrame, 1)

/* Common assertion aliases */
#define TEST_ASSERT(condition)                               UNITY_TEST_ASSERT((condition), __LINE__, " Expression Evaluated To FALSE")
#define TEST_ASSERT_TRUE(condition)                          UNITY_TEST_ASSERT((condition), __LINE__, " Expected TRUE Was FALSE")
#define TEST_ASSERT_FALSE(condition)                         UNITY_TEST_ASSERT(!(condition), __LINE__, " Expected FALSE Was TRUE")
#define TEST_ASSERT_NULL(pointer)                            UNITY_TEST_ASSERT_NULL((pointer), __LINE__, " Expected NULL")
#define TEST_ASSERT_NOT_NULL(pointer)                        UNITY_TEST_ASSERT_NOT_NULL((pointer), __LINE__, " Expected Non-NULL")
#define TEST_ASSERT_EQUAL_INT(expected, actual)              UNITY_TEST_ASSERT_EQUAL_INT((expected), (actual), __LINE__, " Expected Equal")
#define TEST_ASSERT_EQUAL_UINT(expected, actual)             UNITY_TEST_ASSERT_EQUAL_UINT((expected), (actual), __LINE__, " Expected Equal")
#define TEST_ASSERT_EQUAL_INT64(expected, actual)            UNITY_TEST_ASSERT_EQUAL_INT64((expected), (actual), __LINE__, " Expected Equal")
#define TEST_ASSERT_EQUAL_UINT64(expected, actual)           UNITY_TEST_ASSERT_EQUAL_UINT64((expected), (actual), __LINE__, " Expected Equal")
#define TEST_ASSERT_EQUAL_PTR(expected, actual)              UNITY_TEST_ASSERT_EQUAL_PTR((expected), (actual), __LINE__, " Expected Equal")
#define TEST_ASSERT_EQUAL_STRING(expected, actual)           UNITY_TEST_ASSERT_EQUAL_STRING((expected), (actual), __LINE__, " Expected Equal")
#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len)      UNITY_TEST_ASSERT_EQUAL_MEMORY((expected), (actual), (len), __LINE__, " Expected Equal")
#define TEST_FAIL_MESSAGE(message)                           UNITY_TEST_FAIL(__LINE__, (message))

#ifdef __cplusplus
}
#endif

#endif /* UNITY_FRAMEWORK_H */
