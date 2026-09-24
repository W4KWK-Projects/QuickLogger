#pragma once

#include <cstddef>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

// A deliberately tiny test framework, so the suite needs nothing beyond what
// QuickLogger itself already builds with. Each test is a plain function
// registered by QL_TEST; CHECK records a failure and carries on, REQUIRE
// records one and ends that test. tests/test_main.cpp runs them all.

namespace ql
{

    using TestFunction = void (*)();

    struct TestCase
    {
        const char* name;
        TestFunction function;
    };

    std::vector<TestCase>& AllTests();

    // Registers a test at static-initialization time.
    class TestRegistrar
    {
    public:
        TestRegistrar(const char* name, TestFunction function);
    };

    // Records a failed check against the test that's running.
    void RecordFailure(const char* file, int line, const std::string& message);

    // Thrown by REQUIRE to end the current test after a failure.
    struct RequireFailed : std::exception
    {
        [[nodiscard]] const char* what() const noexcept override
        {
            return "REQUIRE failed";
        }
    };

    template <typename T>
    std::string Describe(const T& value)
    {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }

    inline std::string Describe(const std::string& value)
    {
        return "\"" + value + "\"";
    }

    inline std::string Describe(const char* value)
    {
        return value == nullptr ? std::string("nullptr") : "\"" + std::string(value) + "\"";
    }

    inline std::string Describe(bool value)
    {
        return value ? "true" : "false";
    }

}  // namespace ql

#define QL_TEST(name)                                              \
    static void name();                                            \
    static const ql::TestRegistrar name##_registrar(#name, &name); \
    static void name()

#define CHECK(condition)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(condition))                                                   \
        {                                                                   \
            ql::RecordFailure(__FILE__, __LINE__, "CHECK(" #condition ")"); \
        }                                                                   \
    } while (false)

#define REQUIRE(condition)                                                    \
    do                                                                        \
    {                                                                         \
        if (!(condition))                                                     \
        {                                                                     \
            ql::RecordFailure(__FILE__, __LINE__, "REQUIRE(" #condition ")"); \
            throw ql::RequireFailed();                                        \
        }                                                                     \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                \
    do                                                                                            \
    {                                                                                             \
        if (!((actual) == (expected)))                                                            \
        {                                                                                         \
            ql::RecordFailure(__FILE__, __LINE__,                                                 \
                              "CHECK_EQ(" #actual ", " #expected "): got " +                      \
                                  ql::Describe(actual) + ", expected " + ql::Describe(expected)); \
        }                                                                                         \
    } while (false)
