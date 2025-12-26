#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace Rigel::Test {

struct TestFailure : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TestSkip : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry();
void registerTest(const char* name, void (*fn)());

class Registrar {
public:
    Registrar(const char* name, void (*fn)()) {
        registerTest(name, fn);
    }
};

} // namespace Rigel::Test

#define RIGEL_TEST_STRINGIFY_IMPL(x) #x
#define RIGEL_TEST_STRINGIFY(x) RIGEL_TEST_STRINGIFY_IMPL(x)

#define TEST_CASE(name) \
    static void test_##name(); \
    static ::Rigel::Test::Registrar registrar_##name(#name, &test_##name); \
    static void test_##name()

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            throw ::Rigel::Test::TestFailure( \
                std::string("CHECK failed: ") + #expr + " at " + __FILE__ + ":" + RIGEL_TEST_STRINGIFY(__LINE__) \
            ); \
        } \
    } while (false)

#define CHECK_EQ(lhs, rhs) \
    do { \
        auto _lhs = (lhs); \
        auto _rhs = (rhs); \
        if (!(_lhs == _rhs)) { \
            throw ::Rigel::Test::TestFailure( \
                std::string("CHECK_EQ failed: ") + #lhs + " == " + #rhs + " at " + __FILE__ + ":" + RIGEL_TEST_STRINGIFY(__LINE__) \
            ); \
        } \
    } while (false)

#define CHECK_NE(lhs, rhs) \
    do { \
        auto _lhs = (lhs); \
        auto _rhs = (rhs); \
        if (!(_lhs != _rhs)) { \
            throw ::Rigel::Test::TestFailure( \
                std::string("CHECK_NE failed: ") + #lhs + " != " + #rhs + " at " + __FILE__ + ":" + RIGEL_TEST_STRINGIFY(__LINE__) \
            ); \
        } \
    } while (false)

#define CHECK_NEAR(lhs, rhs, eps) \
    do { \
        auto _lhs = (lhs); \
        auto _rhs = (rhs); \
        auto _eps = (eps); \
        if (!((_lhs >= _rhs - _eps) && (_lhs <= _rhs + _eps))) { \
            throw ::Rigel::Test::TestFailure( \
                std::string("CHECK_NEAR failed: ") + #lhs + " ~= " + #rhs + " at " + __FILE__ + ":" + RIGEL_TEST_STRINGIFY(__LINE__) \
            ); \
        } \
    } while (false)

#define CHECK_THROWS(stmt) \
    do { \
        bool _threw = false; \
        try { \
            stmt; \
        } catch (const std::exception&) { \
            _threw = true; \
        } \
        if (!_threw) { \
            throw ::Rigel::Test::TestFailure( \
                std::string("CHECK_THROWS failed: ") + #stmt + " at " + __FILE__ + ":" + RIGEL_TEST_STRINGIFY(__LINE__) \
            ); \
        } \
    } while (false)

#define CHECK_NO_THROW(stmt) \
    do { \
        try { \
            stmt; \
        } catch (const std::exception& e) { \
            throw ::Rigel::Test::TestFailure( \
                std::string("CHECK_NO_THROW failed: ") + #stmt + " at " + __FILE__ + ":" + RIGEL_TEST_STRINGIFY(__LINE__) + \
                " (" + e.what() + ")" \
            ); \
        } \
    } while (false)

#define SKIP_TEST(msg) \
    do { \
        throw ::Rigel::Test::TestSkip(msg); \
    } while (false)
