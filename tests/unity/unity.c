/* =========================================================================
    Unity Project - A Test Framework for C
    Implementation
========================================================================= */

#include "unity.h"
#include <stdio.h>
#include <string.h>

struct UNITY_STORAGE_T Unity;

static const char UnityStrOk[]      = "PASS\n";
static const char UnityStrFail[]    = "FAIL\n";
static const char UnityStrIgnore[]  = "IGNORED\n";

void UnityPrint(const char* string) {
    if (string != NULL) {
        fputs(string, stdout);
    }
}

void UnityPrintNumber(int64_t number) {
    printf("%lld", (long long)number);
}

void UnityPrintNumberUnsigned(uint64_t number) {
    printf("%llu", (unsigned long long)number);
}

void UnityBegin(const char* filename) {
    Unity.TestFile = filename;
    Unity.CurrentTestName = NULL;
    Unity.CurrentTestLineNumber = 0;
    Unity.NumberOfTests = 0;
    Unity.TestFailures = 0;
    Unity.TestIgnores = 0;
    Unity.CurrentTestFailed = 0;
    Unity.CurrentTestIgnored = 0;
}

int UnityEnd(void) {
    UnityPrint("-----------------------\n");
    UnityPrintNumber((int64_t)Unity.NumberOfTests);
    UnityPrint(" Tests ");
    UnityPrintNumber((int64_t)Unity.TestFailures);
    UnityPrint(" Failures ");
    UnityPrintNumber((int64_t)Unity.TestIgnores);
    UnityPrint(" Ignored\n");
    if (Unity.TestFailures == 0) {
        UnityPrint("OK\n");
        return 0;
    }
    UnityPrint("FAIL\n");
    return (int)Unity.TestFailures;
}

void UnityConcludeTest(void) {
    if (Unity.CurrentTestIgnored) {
        Unity.TestIgnores++;
    } else if (!Unity.CurrentTestFailed) {
        UnityPrint(UnityStrOk);
    } else {
        Unity.TestFailures++;
    }
}

void UnityDefaultTestRun(UnityTestFunction Func, const char* FuncName, const int FuncLineNum) {
    Unity.CurrentTestName = FuncName;
    Unity.CurrentTestLineNumber = (size_t)FuncLineNum;
    Unity.NumberOfTests++;
    Unity.CurrentTestFailed = 0;
    Unity.CurrentTestIgnored = 0;

    printf("%s:%d:%s: ", Unity.TestFile, FuncLineNum, FuncName);
    fflush(stdout);

    if (TEST_PROTECT()) {
        setUp();
        Func();
    }
    if (TEST_PROTECT()) {
        tearDown();
    }
    UnityConcludeTest();
}

void UnityFail(const char* msg, size_t line) {
    Unity.CurrentTestFailed = 1;
    printf("%s:%zu: %s (FAIL)", Unity.TestFile, line, msg ? msg : "");
    TEST_ABORT();
}

void UnityIgnore(const char* msg, size_t line) {
    Unity.CurrentTestIgnored = 1;
    printf("%s:%zu: %s (IGNORE)", Unity.TestFile, line, msg ? msg : "");
    TEST_ABORT();
}

void UnityAssertEqualNumber(int64_t expected, int64_t actual, const char* msg, size_t line) {
    if (expected != actual) {
        Unity.CurrentTestFailed = 1;
        printf("\n%s:%zu: Expected %lld Was %lld. %s", Unity.TestFile, line,
               (long long)expected, (long long)actual, msg ? msg : "");
        TEST_ABORT();
    }
}

void UnityAssertEqualString(const char* expected, const char* actual, const char* msg, size_t line) {
    if (expected == NULL && actual == NULL) {
        return;
    }
    if (expected == NULL || actual == NULL || strcmp(expected, actual) != 0) {
        Unity.CurrentTestFailed = 1;
        printf("\n%s:%zu: Expected \"%s\" Was \"%s\". %s", Unity.TestFile, line,
               expected ? expected : "NULL", actual ? actual : "NULL", msg ? msg : "");
        TEST_ABORT();
    }
}

void UnityAssertEqualMemory(const void* expected, const void* actual, size_t length, const char* msg, size_t line) {
    if (length == 0) {
        return;
    }
    if (expected == NULL || actual == NULL) {
        UnityFail("Memory compare with NULL", line);
        return;
    }
    if (memcmp(expected, actual, length) != 0) {
        UnityFail(msg ? msg : "Memory buffers mismatch", line);
    }
}
