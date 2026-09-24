#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "test_framework.hpp"

namespace ql
{

    std::vector<TestCase>& AllTests()
    {
        static std::vector<TestCase> tests;
        return tests;
    }

    TestRegistrar::TestRegistrar(const char* name, TestFunction function)
    {
        AllTests().push_back(TestCase{name, function});
    }

    static int g_failures_in_current_test = 0;

    void RecordFailure(const char* file, int line, const std::string& message)
    {
        ++g_failures_in_current_test;
        std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
    }

    static int RunTests(const char* filter)
    {
        int failed_tests = 0;
        int run = 0;
        for (const TestCase& test : AllTests())
        {
            if (filter != nullptr && std::strstr(test.name, filter) == nullptr)
            {
                continue;
            }
            ++run;
            g_failures_in_current_test = 0;
            try
            {
                test.function();
            }
            catch (const RequireFailed&)
            {
                // Already recorded.
            }
            catch (const std::exception& e)
            {
                RecordFailure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
            }
            if (g_failures_in_current_test > 0)
            {
                ++failed_tests;
                std::fprintf(stderr, "FAIL  %s\n", test.name);
            }
            else
            {
                std::printf("ok    %s\n", test.name);
            }
        }
        std::printf("\n%d test(s), %d failed\n", run, failed_tests);
        return failed_tests == 0 && run > 0 ? 0 : 1;
    }

}  // namespace ql

// Usage: quicklogger_tests [name-substring]
int main(int argc, char** argv)
{
    return ql::RunTests(argc > 1 ? argv[1] : nullptr);
}
