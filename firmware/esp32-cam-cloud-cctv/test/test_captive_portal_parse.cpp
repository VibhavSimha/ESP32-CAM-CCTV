// Host unit tests for the captive-portal login-form auto-detection parser.
//
// This exercises the core promise of issue #33: field names are AUTO-DETECTED,
// never hardcoded. Build & run on a normal machine (no ESP32 toolchain):
//
//   c++ -std=c++11 -I.. test_captive_portal_parse.cpp ../captive_portal_parse.cpp -o /tmp/t && /tmp/t
//
// or simply: ./run_tests.sh

#include "captive_portal_parse.h"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__);            \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// A generic ISP portal that names its fields "user"/"pass" (NOT "username").
static void test_generic_user_pass() {
    std::printf("test_generic_user_pass\n");
    const std::string html =
        "<html><body><form action='/do_login' method='POST'>"
        "<input type='hidden' name='dst' value='http://example.com'>"
        "<input type='text' name='user' placeholder='Login'>"
        "<input type='password' name='pass'>"
        "<input type='submit' value='Connect'>"
        "</form></body></html>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(ok);
    CHECK(f.formFound);
    CHECK(f.valid);
    CHECK(!f.challenge);
    CHECK(f.action == "/do_login");
    CHECK(f.method == "post");
    CHECK(f.userField == "user");   // auto-detected, not hardcoded "username"
    CHECK(f.passField == "pass");
    CHECK(f.hidden.size() == 1);
    CHECK(f.hidden[0].name == "dst");

    std::string body = buildFormBody(f, "alice", "s3cret");
    CHECK(body.find("user=alice") != std::string::npos);
    CHECK(body.find("pass=s3cret") != std::string::npos);
    CHECK(body.find("dst=") != std::string::npos);
}

// An email-style login where the first input is type=email and named oddly.
static void test_email_field() {
    std::printf("test_email_field\n");
    const std::string html =
        "<form method=\"post\" action=\"http://10.0.0.1/auth\">"
        "<input type=\"email\" name=\"j_username\">"
        "<input type=\"password\" name=\"j_password\">"
        "</form>";
    PortalForm f;
    CHECK(parseLoginForm(html, f));
    CHECK(f.userField == "j_username");
    CHECK(f.passField == "j_password");
    CHECK(f.action == "http://10.0.0.1/auth");
}

// Input with no explicit type attribute defaults to text (username).
static void test_default_text_type() {
    std::printf("test_default_text_type\n");
    const std::string html =
        "<form action='/login'>"
        "<input name='login_id'>"
        "<input type='password' name='pw'>"
        "</form>";
    PortalForm f;
    CHECK(parseLoginForm(html, f));
    CHECK(f.userField == "login_id");
    CHECK(f.passField == "pw");
    CHECK(f.method == "get"); // no method attr -> HTML default
}

// MikroTik CHAP challenge portal must be flagged unsupported (fallback path).
static void test_mikrotik_chap_fallback() {
    std::printf("test_mikrotik_chap_fallback\n");
    const std::string html =
        "<form name='login' action='http://10.201.125.1/login' method='post'>"
        "<input type='hidden' name='dst' value='http://x'>"
        "<input type='hidden' name='popup' value='true'>"
        "<input type='hidden' name='chap-id' value='\\5c'>"
        "<input type='hidden' name='chap-challenge' value='abc'>"
        "<input name='username' type='text'>"
        "<input name='password' type='password'>"
        "</form>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(!ok);           // not automatable
    CHECK(f.formFound);
    CHECK(f.challenge);   // challenge portal detected
    CHECK(!f.valid);
}

// A JavaScript-only portal (no <form>) must fall back gracefully.
static void test_no_form_fallback() {
    std::printf("test_no_form_fallback\n");
    const std::string html =
        "<html><body><div id='app'></div>"
        "<script>renderLogin();</script></body></html>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(!ok);
    CHECK(!f.formFound);
}

// Attribute order and extra whitespace should not confuse detection; the first
// text-like input wins even when it appears after hidden inputs.
static void test_attribute_order() {
    std::printf("test_attribute_order\n");
    const std::string html =
        "<form   action = '/l'   method = 'PoSt' >"
        "<input value='keepme' name='token' type='hidden'>"
        "<input placeholder='Name' name='acct' type='text' class='x'>"
        "<input class='y' type='password' name='secret'>"
        "</form>";
    PortalForm f;
    CHECK(parseLoginForm(html, f));
    CHECK(f.method == "post");
    CHECK(f.userField == "acct");
    CHECK(f.passField == "secret");
    CHECK(f.hidden.size() == 1);
    CHECK(f.hidden[0].name == "token");
    CHECK(f.hidden[0].value == "keepme");
}

static void test_captive_detection() {
    std::printf("test_captive_detection\n");
    CHECK(!looksLikeCaptivePortal(204, ""));            // clean pass-through
    CHECK(looksLikeCaptivePortal(302, ""));             // redirect to portal
    CHECK(looksLikeCaptivePortal(200, "<html>login</html>")); // portal page
    CHECK(looksLikeCaptivePortal(204, "unexpected"));   // 204 but with body
    CHECK(!looksLikeCaptivePortal(200, ""));            // empty 200, no redirect
}

int main() {
    test_generic_user_pass();
    test_email_field();
    test_default_text_type();
    test_mikrotik_chap_fallback();
    test_no_form_fallback();
    test_attribute_order();
    test_captive_detection();

    if (g_failures == 0) {
        std::printf("\nAll captive-portal parser tests passed.\n");
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
