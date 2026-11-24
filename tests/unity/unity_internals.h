/* =========================================================================
    Unity Project - A Test Framework for C
    Internals and macro definitions
========================================================================= */

#ifndef UNITY_INTERNALS_H
#define UNITY_INTERNALS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef void (*UnityTestFunction)(void);

struct UNITY_STORAGE_T {
    const char* TestFile;
    const char* CurrentTestName;
    size_t CurrentTestLineNumber;
    size_t NumberOfTests;
    size_t TestFailures;
    size_t TestIgnores;
    size_t CurrentTestFailed;
    size_t CurrentTestIgnored;
    jmp_buf AbortFrame;
};

extern struct UNITY_STORAGE_T Unity;

void UnityPrint(const char* string);
void UnityPrintNumber(int64_t number);
void UnityPrintNumberUnsigned(uint64_t number);
void UnityConcludeTest(void);

void UnityAssertEqualNumber(int64_t expected, int64_t actual, const char* msg, size_t line);
void UnityAssertEqualIntArray(const int* expected, const int* actual, size_t num_elements, const char* msg, size_t line);
void UnityAssertBits(uint64_t mask, uint64_t expected, uint64_t actual, const char* msg, size_t line);
void UnityAssertEqualString(const char* expected, const char* actual, const char* msg, size_t line);
void UnityAssertEqualMemory(const void* expected, const void* actual, size_t length, const char* msg, size_t line);
void UnityFail(const char* msg, size_t line);
void UnityIgnore(const char* msg, size_t line);

#define UNITY_TEST_ASSERT(condition, line, message) \
    if (!(condition)) { UnityFail((message), (size_t)(line)); }

#define UNITY_TEST_ASSERT_NULL(pointer, line, message) \
    UNITY_TEST_ASSERT((pointer) == NULL, line, message)

#define UNITY_TEST_ASSERT_NOT_NULL(pointer, line, message) \
    UNITY_TEST_ASSERT((pointer) != NULL, line, message)

#define UNITY_TEST_ASSERT_EQUAL_INT(expected, actual, line, message) \
    UnityAssertEqualNumber((int64_t)(expected), (int64_t)(actual), (message), (size_t)(line))

#define UNITY_TEST_ASSERT_EQUAL_UINT(expected, actual, line, message) \
    UnityAssertEqualNumber((int64_t)(expected), (int64_t)(actual), (message), (size_t)(line))

#define UNITY_TEST_ASSERT_EQUAL_INT64(expected, actual, line, message) \
    UnityAssertEqualNumber((int64_t)(expected), (int64_t)(actual), (message), (size_t)(line))

#define UNITY_TEST_ASSERT_EQUAL_UINT64(expected, actual, line, message) \
    UnityAssertEqualNumber((int64_t)(expected), (int64_t)(actual), (message), (size_t)(line))

#define UNITY_TEST_ASSERT_EQUAL_PTR(expected, actual, line, message) \
    UnityAssertEqualNumber((intptr_t)(expected), (intptr_t)(actual), (message), (size_t)(line))

#define UNITY_TEST_ASSERT_EQUAL_STRING(expected, actual, line, message) \
    UnityAssertEqualString((const char*)(expected), (const char*)(actual), (message), (size_t)(line))

#define UNITY_TEST_ASSERT_EQUAL_MEMORY(expected, actual, len, line, message) \
    UnityAssertEqualMemory((const void*)(expected), (const void*)(actual), (size_t)(len), (message), (size_t)(line))

#define UNITY_TEST_FAIL(line, message) \
    UnityFail((message), (size_t)(line))

#ifdef __cplusplus
}
#endif

#endif /* UNITY_INTERNALS_H */
