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

// MikroTik CHAP portal (chap-id + chap-challenge hidden fields) must now be
// AUTOMATED: the parser decodes the hex challenge, marks the form automatable
// via CHAP, and buildFormBody submits the MD5 response — never the plaintext
// password (issue #42).
static void test_mikrotik_chap_automated() {
    std::printf("test_mikrotik_chap_automated\n");
    const std::string html =
        "<form name='login' action='http://10.201.125.1/login' method='post'>"
        "<input type='hidden' name='dst' value='http://x'>"
        "<input type='hidden' name='popup' value='true'>"
        "<input type='hidden' name='chap-id' value='0a'>"
        "<input type='hidden' name='chap-challenge' value='1234567890abcdef1234567890abcdef'>"
        "<input name='username' type='text'>"
        "<input name='password' type='password'>"
        "</form>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(ok);            // now automatable
    CHECK(f.formFound);
    CHECK(f.chapLogin);   // MikroTik CHAP auto-login path
    CHECK(f.valid);
    CHECK(!f.challenge);
    CHECK(f.userField == "username");
    CHECK(f.passField == "password");

    // Precomputed reference: md5( \x0a + "test123" + fromhex(challenge) ).
    const std::string expected = "e24fef6ab8c4544904d4e616601543c3";
    std::string body = buildFormBody(f, "alice", "test123");
    CHECK(body.find("password=" + expected) != std::string::npos);   // CHAP MD5 response submitted
    CHECK(body.find("test123") == std::string::npos);                 // plaintext password never sent
    CHECK(body.find("username=alice") != std::string::npos);
    CHECK(body.find("dst=") != std::string::npos);
}

// A CHAP portal we cannot decode (odd/garbled chap-challenge) must fall back to
// the manual browser flow rather than submitting a wrong hash.
static void test_mikrotik_chap_incomplete_fallback() {
    std::printf("test_mikrotik_chap_incomplete_fallback\n");
    const std::string html =
        "<form name='login' action='http://10.201.125.1/login' method='post'>"
        "<input type='hidden' name='chap-id' value='0a'>"
        "<input type='hidden' name='chap-challenge' value='xyz'>" // not hex
        "<input name='username' type='text'>"
        "<input name='password' type='password'>"
        "</form>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(!ok);          // cannot be automated safely
    CHECK(f.formFound);
    CHECK(f.challenge);  // -> manual browser fallback
    CHECK(!f.chapLogin);
    CHECK(!f.valid);
}

// A plain login form that merely has a stray field named "chap-id" (but no
// chap-challenge) is NOT a CHAP portal: it must stay a normal, automatable
// username/password form and not be pushed into the CHAP fallback.
static void test_stray_chap_id_is_plain_form() {
    std::printf("test_stray_chap_id_is_plain_form\n");
    const std::string html =
        "<form action='/login' method='post'>"
        "<input type='hidden' name='chap-id' value='0a'>"
        "<input name='username' type='text'>"
        "<input type='password' name='password'>"
        "</form>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(ok);
    CHECK(f.valid);
    CHECK(!f.chapLogin);
    CHECK(!f.challenge);
    CHECK(f.userField == "username");
    CHECK(f.passField == "password");
}

// MD5 and the MikroTik CHAP password construction against known-good vectors.
static void test_md5_chap_vectors() {
    std::printf("test_md5_chap_vectors\n");
    // With empty chap-id/chap-challenge, mikrotikChapPassword() reduces to a
    // plain MD5 of the password — the canonical RFC 1321 test vector for "abc".
    CHECK(mikrotikChapPassword("", "", "abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(mikrotikChapPassword("", "", "") == "d41d8cd98f00b204e9800998ecf8427e");
    // A message long enough to span two MD5 blocks (100 'A's) exercises padding.
    CHECK(mikrotikChapPassword("", "", std::string(100, 'A')) ==
          "8adc5937e635f6c9af646f0b23560fae");
    // Full CHAP construction: chap-id 0x0a, "test123", 16-byte challenge.
    std::string idBytes(1, (char)0x0a);
    std::string challenge;
    for (int i = 0; i < 2; i++) {
        const unsigned char b[8] = {0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd, 0xef};
        challenge.append((const char*)b, 8);
    }
    CHECK(mikrotikChapPassword(idBytes, challenge, "test123") ==
          "e24fef6ab8c4544904d4e616601543c3");
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

// Issue #42 (follows on from #40): the exact field-report scenario — the
// generate_204 probe is intercepted with an HTTP 302 to a MikroTik ISP portal.
// Its login page is a CHAP form; instead of giving up, the firmware now decodes
// chap-id/chap-challenge and logs in by submitting MD5(chap-id+password+
// chap-challenge). Cloud uploads still stay paused (captivePortalIsOnline()==
// false) until the post-login re-probe confirms connectivity — the #40 guarantee
// is unchanged; only the path to clearing the portal is now automatic.
static void test_issue42_mikrotik_chap_login() {
    std::printf("test_issue42_mikrotik_chap_login\n");
    // 1. The probe response looks like a captive portal (302 redirect).
    CHECK(looksLikeCaptivePortal(302, ""));

    // 2. The fetched MikroTik portal page is a CHAP form with hex-encoded
    //    chap-id/chap-challenge -> now automatable end to end.
    const std::string portalHtml =
        "<html><head><title>ISP Login</title></head><body>"
        "<form name='login' action='http://10.201.125.1/login' method='post'>"
        "<input type='hidden' name='dst' value='http://connectivitycheck.gstatic.com/generate_204'>"
        "<input type='hidden' name='popup' value='true'>"
        "<input type='hidden' name='chap-id' value='37'>"
        "<input type='hidden' name='chap-challenge' value='deadbeefdeadbeefdeadbeefdeadbeef'>"
        "<input name='username' type='text'>"
        "<input name='password' type='password'>"
        "</form></body></html>";
    PortalForm f;
    bool ok = parseLoginForm(portalHtml, f);
    CHECK(ok);             // automatable via CHAP
    CHECK(f.formFound);
    CHECK(f.chapLogin);
    CHECK(f.valid);
    CHECK(!f.challenge);

    // The submitted password is the 32-hex-char CHAP response, not the plaintext.
    std::string body = buildFormBody(f, "guest", "hunter2");
    CHECK(body.find("hunter2") == std::string::npos);
    CHECK(body.find("username=guest") != std::string::npos);
    CHECK(body.find("popup=true") != std::string::npos);
}

// Issue #44: the MikroTik DEFAULT hotspot login page. A hidden "sendin" form is
// emitted BEFORE the visible username/password form, and the chap-id/chap-
// challenge live ONLY in the md5.js hexMD5(...) call — not as <input>s. The
// parser must scan past the stub form, find the real one, and read the CHAP
// tokens from the JavaScript so the login is automated end to end.
static void test_issue44_mikrotik_default_template_chap_js() {
    std::printf("test_issue44_mikrotik_default_template_chap_js\n");
    const std::string html =
        "<html><head>"
        "<meta http-equiv=\"refresh\" content=\"5; url=http://10.201.125.1/status\">"
        "</head><body>"
        "<form name=\"sendin\" action=\"http://10.201.125.1/login\" method=\"post\">"
        "<input type=\"hidden\" name=\"username\" />"
        "<input type=\"hidden\" name=\"password\" />"
        "<input type=\"hidden\" name=\"dst\" value=\"http://connectivitycheck.gstatic.com/generate_204\" />"
        "<input type=\"hidden\" name=\"popup\" value=\"true\" />"
        "</form>"
        "<script type=\"text/javascript\" src=\"/md5.js\"></script>"
        "<script type=\"text/javascript\">"
        "function doLogin() {"
        "document.sendin.username.value = document.login.username.value;"
        "document.sendin.password.value = hexMD5('37' + document.login.password.value + 'deadbeefdeadbeefdeadbeefdeadbeef');"
        "document.sendin.submit(); return false; }"
        "</script>"
        "<form name=\"login\" action=\"http://10.201.125.1/login\" method=\"post\" onSubmit=\"return doLogin()\">"
        "<input type=\"hidden\" name=\"dst\" value=\"http://connectivitycheck.gstatic.com/generate_204\" />"
        "<input type=\"hidden\" name=\"popup\" value=\"true\" />"
        "<input name=\"username\" type=\"text\" value=\"\"/>"
        "<input name=\"password\" type=\"password\"/>"
        "<input type=\"submit\" value=\"OK\" />"
        "</form></body></html>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(ok);
    CHECK(f.formFound);
    CHECK(f.valid);
    CHECK(f.chapLogin);                  // read from the md5.js hexMD5(...) call
    CHECK(!f.challenge);
    CHECK(f.userField == "username");    // from the VISIBLE login form, not sendin
    CHECK(f.passField == "password");
    CHECK(f.action == "http://10.201.125.1/login");
    CHECK(f.chapId.size() == 1);         // 0x37 -> one byte
    CHECK(f.chapChallenge.size() == 16); // 32 hex chars -> 16 bytes

    std::string idBytes(1, (char)0x37);
    std::string challenge;
    const unsigned char dead[4] = {0xde, 0xad, 0xbe, 0xef};
    for (int i = 0; i < 4; i++) challenge.append((const char*)dead, 4);
    const std::string expected = mikrotikChapPassword(idBytes, challenge, "hunter2");

    std::string body = buildFormBody(f, "guest", "hunter2");
    CHECK(body.find("hunter2") == std::string::npos);   // plaintext never sent
    CHECK(body.find(expected) != std::string::npos);    // CHAP MD5 response submitted
    CHECK(body.find("username=guest") != std::string::npos);
    CHECK(body.find("popup=true") != std::string::npos);
}

// Issue #44: a MikroTik rlogin-style LANDING page. It is not the login form — it
// only redirects (via <meta refresh>) to the real login page and offers a
// "continue" button. parseLoginForm must report no usable form but hand back the
// meta-refresh URL so the caller can follow one hop to the real login page.
static void test_redirect_page_meta_refresh() {
    std::printf("test_redirect_page_meta_refresh\n");
    const std::string html =
        "<html><head>"
        "<meta http-equiv=\"refresh\" content=\"3; url=http://10.201.125.1/login?dst=http://x\">"
        "</head><body>"
        "If you are not redirected in a few seconds, click 'continue' below<br>"
        "<form action=\"http://10.201.125.1/login\" method=\"get\">"
        "<input type=\"submit\" value=\"continue\"></form>"
        "</body></html>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(!ok);
    CHECK(f.formFound);       // a <form> exists (the "continue" button)
    CHECK(!f.valid);
    CHECK(f.redirectUrl == "http://10.201.125.1/login?dst=http://x");
}

// A JavaScript-only redirect landing page (no <form>) must still surface the
// window.location target as the next hop.
static void test_redirect_page_js_location() {
    std::printf("test_redirect_page_js_location\n");
    const std::string html =
        "<html><head></head><body>Redirecting..."
        "<script>window.location.href = \"/login?mac=AA-BB\";</script>"
        "</body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(!f.formFound);
    CHECK(f.redirectUrl == "/login?mac=AA-BB");
}

// A bare `window.location='…'` assignment (no .href/.replace) is still followed.
static void test_redirect_js_bare_window_location() {
    std::printf("test_redirect_js_bare_window_location\n");
    const std::string html =
        "<html><body>"
        "<script>window.location='/hotspot/login.html';</script>"
        "</body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(!f.formFound);
    CHECK(f.redirectUrl == "/hotspot/login.html");
}

// A "location=" that is only a QUERY-STRING parameter inside a quoted URL (not a
// JS redirect) must NOT be mistaken for a next-hop target. There is no <meta>
// refresh, no <form> and no continue/login anchor, so the redirect must be empty.
// (The trailing attribute makes a naive bare-"location=" scan return bogus text,
// so this also guards against reintroducing that too-broad key.)
static void test_redirect_ignores_querystring_location() {
    std::printf("test_redirect_ignores_querystring_location\n");
    const std::string html =
        "<html><body>Loading..."
        "<img src=\"/pixel.png?location=home\" alt=\"banner\">"
        "</body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(!f.formFound);
    CHECK(f.redirectUrl.empty());
}

// A stray "location.href" token (e.g. in JSON/config text) is NOT a redirect.
// Only real JS assignment/call syntax should produce a redirectUrl.
static void test_redirect_ignores_non_redirect_location_href_token() {
    std::printf("test_redirect_ignores_non_redirect_location_href_token\n");
    const std::string html =
        "<html><body>"
        "<script>var cfg={\"location.href\":\"https://alpsmp.spectra.co/alepocp/?server=x\"};</script>"
        "</body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(!f.formFound);
    CHECK(f.redirectUrl.empty());
}

// A landing page whose only next-hop hint is a "continue"/login anchor.
static void test_redirect_page_continue_link() {
    std::printf("test_redirect_page_continue_link\n");
    const std::string html =
        "<html><body><p>Please wait. "
        "<a href=\"/login?token=1\">click continue to log in</a></p></body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(f.redirectUrl == "/login?token=1");
}

// A generic page with a "Log in" link (e.g. WordPress chrome) is NOT a captive-
// portal landing redirect by itself. Without an explicit redirect cue, this link
// must not be treated as the next hop.
static void test_redirect_ignores_generic_log_in_anchor() {
    std::printf("test_redirect_ignores_generic_log_in_anchor\n");
    const std::string html =
        "<html><body>"
        "<form action=\"/search\" method=\"get\"><input type=\"search\" name=\"s\"></form>"
        "<a href=\"https://alpsmp.spectra.co/alepocp/wp-login.php\">Log in</a>"
        "</body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(f.formFound);            // page has a form, but not a login form
    CHECK(!f.valid);
    CHECK(f.redirectUrl.empty());  // do not follow unrelated "Log in" chrome
}

// A page with a stub form FIRST (a bare submit button) and the real
// username/password form second: the login form must win, not the first form.
static void test_multi_form_picks_login_form() {
    std::printf("test_multi_form_picks_login_form\n");
    const std::string html =
        "<form action=\"/next\" method=\"get\"><input type=\"submit\" value=\"Enter\"></form>"
        "<form action=\"/auth\" method=\"post\">"
        "<input name=\"u\" type=\"text\"><input name=\"p\" type=\"password\"></form>";
    PortalForm f;
    CHECK(parseLoginForm(html, f));
    CHECK(f.valid);
    CHECK(!f.chapLogin);
    CHECK(f.userField == "u");
    CHECK(f.passField == "p");
    CHECK(f.action == "/auth");
}

// A visible username/password form whose md5.js CHAP tokens will NOT decode
// (non-hex) must fall back to the manual browser path — never send the plaintext
// password to a CHAP portal.
static void test_js_chap_undecodable_is_challenge() {
    std::printf("test_js_chap_undecodable_is_challenge\n");
    const std::string html =
        "<form name=\"login\" action=\"/login\" method=\"post\">"
        "<input name=\"username\" type=\"text\">"
        "<input name=\"password\" type=\"password\"></form>"
        "<script>x = hexMD5('zz' + document.login.password.value + 'nothex');</script>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(!ok);
    CHECK(f.challenge);
    CHECK(!f.valid);
    CHECK(!f.chapLogin);
}

// Issue #46: the exact field-reported page — a MikroTik/RADIUSdesk "redirect"
// LANDING page ("If you are not redirected in a few seconds, click 'continue'
// below") whose only actionable element is a <form name="redirect" method="post">
// that the browser auto-submits (document.redirect.submit()) to reach the real
// login page. There is no <meta refresh>, no JS location change and no "continue"
// <a> link, so the ONLY next-hop hint is the POST form. Previously this produced
// redirect:'' and dead-ended; now the POST action, method and hidden fields are
// surfaced so the firmware can follow the hop the same way the browser would.
static void test_issue46_mikrotik_redirect_form_post() {
    std::printf("test_issue46_mikrotik_redirect_form_post\n");
    const std::string html =
        "<html><body>"
        "<center>If you are not redirected in a few seconds, click 'continue' below<br>"
        "<form name=\"redirect\" action=\"http://portal.example.net/rd/mikrotik-browser-detect\" method=\"post\">"
        "<input type=\"hidden\" name=\"mac\" value=\"A8:42:E3:48:53:0C\">"
        "<input type=\"hidden\" name=\"ip\" value=\"10.201.125.99\">"
        "<input type=\"hidden\" name=\"loginlink\" value=\"http://10.201.125.1/login\">"
        "<input type=\"hidden\" name=\"nasid\" value=\"hotspot1\">"
        "<input type=\"submit\" value=\"continue\">"
        "</form></center>"
        "<script type=\"text/javascript\">document.redirect.submit();</script>"
        "</body></html>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(!ok);                 // not an automatable username/password form
    CHECK(f.formFound);         // a <form> exists (the redirect form)
    CHECK(!f.valid);
    CHECK(!f.challenge);
    CHECK(!f.chapLogin);
    // The next hop is the POST redirect form — its action, method and hidden
    // fields must all be surfaced (this is what was missing: redirect:'').
    CHECK(f.redirectUrl == "http://portal.example.net/rd/mikrotik-browser-detect");
    CHECK(f.redirectMethod == "post");
    CHECK(f.redirectFields.size() == 4);   // mac, ip, loginlink, nasid (submit excluded)
    CHECK(f.redirectFields[0].name == "mac");
    CHECK(f.redirectFields[2].name == "loginlink");
    CHECK(f.redirectFields[2].value == "http://10.201.125.1/login");

    // The body the firmware POSTs to follow the hop echoes every hidden field
    // (buildFormBody with empty user/pass emits only the hidden fields).
    PortalForm hop;
    hop.hidden = f.redirectFields;
    std::string body = buildFormBody(hop, "", "");
    CHECK(body.find("mac=A8%3A42%3AE3%3A48%3A53%3A0C") != std::string::npos);
    CHECK(body.find("loginlink=http%3A%2F%2F10.201.125.1%2Flogin") != std::string::npos);
}

// A GET-method MikroTik "redirect" landing form (the PacketFence variant) must
// likewise be surfaced as the next hop, with redirectMethod=="get".
static void test_issue46_mikrotik_redirect_form_get() {
    std::printf("test_issue46_mikrotik_redirect_form_get\n");
    const std::string html =
        "<html><body>"
        "<center>If you are not redirected in a few seconds, click 'continue' below<br>"
        "<form name=\"redirect\" action=\"http://192.168.1.5/Mikrotik\" method=\"get\">"
        "<input type=\"hidden\" name=\"mac\" value=\"00:11:22:33:44:55\">"
        "<input type=\"hidden\" name=\"ip\" value=\"10.0.0.9\">"
        "<input type=\"submit\" value=\"continue\">"
        "</form></center></body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(f.formFound);
    CHECK(!f.valid);
    CHECK(f.redirectUrl == "http://192.168.1.5/Mikrotik");
    CHECK(f.redirectMethod == "get");
    CHECK(f.redirectFields.size() == 2);
}

// A <meta refresh> present ALONGSIDE a POST redirect form must still win (a meta
// refresh is a plain GET navigation): redirectMethod stays empty so the caller
// GETs the meta target rather than POSTing the form.
static void test_issue46_meta_refresh_beats_redirect_form() {
    std::printf("test_issue46_meta_refresh_beats_redirect_form\n");
    const std::string html =
        "<html><head>"
        "<meta http-equiv=\"refresh\" content=\"0; url=http://10.201.125.1/login?dst=x\">"
        "</head><body>"
        "<form name=\"redirect\" action=\"http://detect.example/\" method=\"post\">"
        "<input type=\"hidden\" name=\"mac\" value=\"00:11:22:33:44:55\"></form>"
        "</body></html>";
    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(f.redirectUrl == "http://10.201.125.1/login?dst=x");
    CHECK(f.redirectMethod.empty());        // GET navigation, not a POST hop
    CHECK(f.redirectFields.empty());
}

// Issue #48 — the Spectra (RADIUSdesk/MikroTik external-portal) landing page.
// The plain-HTTP rlogin landing page the hotspot serves after interception is a
// browser-auto-submitted POST form whose action is an **https://** external
// portal. The parser must surface it as a POST next hop with an https redirect
// URL; the firmware then has to follow that hop over TLS (a plain WiFiClient
// returns HTTPC_ERROR_CONNECTION_LOST (-5) and dead-ends — the root-cause bug
// fixed by using WiFiClientSecure for https portal URLs). This locks in the
// parser side of that scenario.
static void test_issue48_spectra_https_redirect_post() {
    std::printf("test_issue48_spectra_https_redirect_post\n");
    const std::string html =
        "<html><head><title>Spectra</title></head>"
        "<body onload=\"document.forms[0].submit()\">"
        "<center>If you are not redirected in a few seconds, click 'continue' below<br>"
        "<form action=\"https://alpsmp.spectra.co/alepocp/\" method=\"post\">"
        "<input type=\"hidden\" name=\"mac\" value=\"A8:42:E3:48:53:0C\">"
        "<input type=\"hidden\" name=\"ip\" value=\"10.201.125.99\">"
        "<input type=\"hidden\" name=\"link-login-only\" value=\"http://10.201.125.1/login\">"
        "<input type=\"hidden\" name=\"link-orig\" value=\"http://connectivitycheck.gstatic.com/generate_204\">"
        "<input type=\"submit\" value=\"continue\">"
        "</form></center></body></html>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(!ok);                 // no automatable username/password form on the landing page
    CHECK(f.formFound);         // the redirect <form> is present
    CHECK(!f.valid);
    CHECK(!f.challenge);
    CHECK(!f.chapLogin);
    // The next hop is the POST redirect to the HTTPS external portal.
    CHECK(f.redirectUrl == "https://alpsmp.spectra.co/alepocp/");
    CHECK(f.redirectUrl.rfind("https://", 0) == 0);   // scheme requires TLS transport (the fix)
    CHECK(f.redirectMethod == "post");
    CHECK(f.redirectFields.size() == 4);              // mac, ip, link-login-only, link-orig
    CHECK(f.redirectFields[0].name == "mac");
    CHECK(f.redirectFields[2].name == "link-login-only");
    CHECK(f.redirectFields[2].value == "http://10.201.125.1/login");

    // The body the firmware POSTs to follow the hop echoes every hidden field.
    PortalForm hop;
    hop.hidden = f.redirectFields;
    std::string body = buildFormBody(hop, "", "");
    CHECK(body.find("mac=A8%3A42%3AE3%3A48%3A53%3A0C") != std::string::npos);
    CHECK(body.find("link-login-only=http%3A%2F%2F10.201.125.1%2Flogin") != std::string::npos);
}

// Issue #53 — Spectra landing page captured from the failing ESP32 logs.
// It is a browser-auto-submitted POST form with many hidden fields (including
// empty ones like username/chap-id/chap-challenge). The parser must expose this
// as a POST redirect hop and preserve every hidden field so the firmware can
// replay the same submit before parsing the real login page.
static void test_issue53_spectra_landing_form_full_hidden_echo() {
    std::printf("test_issue53_spectra_landing_form_full_hidden_echo\n");
    const std::string html =
        "<html><head><title>...</title></head><body>"
        "<center>If you are not redirected in a few seconds, click 'continue' below<br>"
        "<form name=\"redirect\" action=\"https://alpsmp.spectra.co/alepocp/?server=HS_STNZ_BAN_LFYTE_DATA&CM=A8:42:E3:48:53:0C&ip=10.201.125.127\" method=\"post\">"
        "<input type=\"hidden\" name=\"CM\" value=\"A8:42:E3:48:53:0C\">"
        "<input type=\"hidden\" name=\"ip\" value=\"10.201.125.127\">"
        "<input type=\"hidden\" name=\"username\" value=\"\">"
        "<input type=\"hidden\" name=\"link-login\" value=\"http://10.201.125.1/login?dst=http%3A%2F%2Fconnectivitycheck.gstatic.com%2Fgenerate%5F204\">"
        "<input type=\"hidden\" name=\"link-orig\" value=\"http://connectivitycheck.gstatic.com/generate_204\">"
        "<input type=\"hidden\" name=\"error\" value=\"\">"
        "<input type=\"hidden\" name=\"chap-id\" value=\"\">"
        "<input type=\"hidden\" name=\"chap-challenge\" value=\"\">"
        "<input type=\"hidden\" name=\"link-login-only\" value=\"http://10.201.125.1/login\">"
        "<input type=\"hidden\" name=\"link-orig-esc\" value=\"http%3A%2F%2Fconnectivitycheck.gstatic.com%2Fgenerate%5F204\">"
        "<input type=\"hidden\" name=\"mac-esc\" value=\"A8%3A42%3AE3%3A48%3A53%3A0C\">"
        "<input type=\"hidden\" name=\"nasid\" value=\"STNZ_BAN_LFYTE_M75\">"
        "<input type=\"hidden\" name=\"server\" value=\"HS_STNZ_BAN_LFYTE_DATA\">"
        "<input type=\"submit\" value=\"continue\">"
        "</form></center></body></html>";

    PortalForm f;
    CHECK(!parseLoginForm(html, f));
    CHECK(f.formFound);
    CHECK(!f.valid);
    CHECK(f.redirectMethod == "post");
    CHECK(f.redirectUrl ==
          "https://alpsmp.spectra.co/alepocp/?server=HS_STNZ_BAN_LFYTE_DATA&CM=A8:42:E3:48:53:0C&ip=10.201.125.127");
    CHECK(f.redirectFields.size() == 13);

    PortalForm hop;
    hop.hidden = f.redirectFields;
    std::string body = buildFormBody(hop, "", "");
    CHECK(body.find("CM=A8%3A42%3AE3%3A48%3A53%3A0C") != std::string::npos);
    CHECK(body.find("username=") != std::string::npos);
    CHECK(body.find("chap-id=") != std::string::npos);
    CHECK(body.find("chap-challenge=") != std::string::npos);
    CHECK(body.find("link-orig-esc=http%253A%252F%252Fconnectivitycheck.gstatic.com%252Fgenerate%255F204") != std::string::npos);
}

// Issue #50 — Spectra/ALEPO portal login is submitted by JavaScript to
// wp-admin/admin-ajax.php and may use a PIN field that is text-like (not
// type=password). The parser should still produce an automatable form with the
// correct action URL and credential field mapping.
static void test_issue50_spectra_ajax_pin_login() {
    std::printf("test_issue50_spectra_ajax_pin_login\n");
    const std::string html =
        "<html><body>"
        "<form id='existing-user-login' method='post'>"
        "<input type='hidden' name='action' value='existing_user_credentials_submit'>"
        "<input type='hidden' name='existing_user_login_nonse' value='529e700beb'>"
        "<input type='hidden' name='_wp_http_referer' value='/alepocp/stanza/?CM=A8%3A42&ip=10.0.0.2&ref='>"
        "<input type='hidden' name='postedForm' value='y'>"
        "<input type='text' name='error_message_incorrect_userid_password' value='Invalid Credentials.' style='display:none !important;'>"
        "<input type='text' name='error_message_couldnt_login_user' value='Could not Login user. Please contact System Administrator.' style='display:none !important;'>"
        "<input type='text' name='extuser_device_type'>"
        "<input type='text' name='existing_userId'>"
        "<input type='text' name='pin'>"
        "<button type='submit'>Log in</button>"
        "</form>"
        "<script>"
        "$.ajax({url:'https:\\/\\/alpsmp.spectra.co\\/alepocp\\/wp-admin\\/admin-ajax.php',type:'POST'});"
        "</script>"
        "</body></html>";
    PortalForm f;
    bool ok = parseLoginForm(html, f);
    CHECK(ok);
    CHECK(f.formFound);
    CHECK(f.valid);
    CHECK(!f.challenge);
    CHECK(f.userField == "existing_userId");
    CHECK(f.passField == "pin");
    CHECK(f.deviceTypeField == "extuser_device_type");
    CHECK(f.method == "post");
    CHECK(f.action == "https://alpsmp.spectra.co/alepocp/wp-admin/admin-ajax.php");

    std::string body = buildFormBody(f, "STN-26LFTBA055", "2272", "esp32-cam-48530C");
    CHECK(body.find("action=existing_user_credentials_submit") != std::string::npos);
    CHECK(body.find("existing_userId=STN-26LFTBA055") != std::string::npos);
    CHECK(body.find("pin=2272") != std::string::npos);
    CHECK(body.find("extuser_device_type=esp32-cam-48530C") != std::string::npos);
    CHECK(body.find("existing_user_login_nonse=529e700beb") != std::string::npos);
}

// Issue #55 — Spectra's real login form includes hidden-by-style text inputs that
// carry static error messages. They are not operator-editable credentials and must
// not be picked as the username field.
static void test_issue55_spectra_ignores_hidden_error_text_inputs() {
    std::printf("test_issue55_spectra_ignores_hidden_error_text_inputs\n");
    const std::string html =
        "<html><body>"
        "<form method='POST' id='alepo-existing_user-form' action=/alepocp/stanza/?CM=A8&ip=10.0.0.2&ref= class='alepo-ajax-form'>"
        "<input type='hidden' class='alepo-ajax-action' name='action' value='existing_user_credentials_submit'>"
        "<input type='text' name='error_message_incorrect_userid_password' value='Invalid Credentials.' style='display:none !important;'>"
        "<input type='text' name='error_message_user_not_found' value='Invalid Credentials.' style='display:none !important;'>"
        "<input required='required' type='text' name='existing_userId' value=''>"
        "<input required='required' type='password' name='pin' value=''>"
        "</form>"
        "<script>var alepo={\"alepourl\":\"https:\\/\\/alpsmp.spectra.co\\/alepocp\\/wp-admin\\/admin-ajax.php\"};</script>"
        "</body></html>";
    PortalForm f;
    CHECK(parseLoginForm(html, f));
    CHECK(f.formFound);
    CHECK(f.valid);
    CHECK(!f.challenge);
    CHECK(f.userField == "existing_userId");
    CHECK(f.passField == "pin");
    CHECK(f.action == "https://alpsmp.spectra.co/alepocp/wp-admin/admin-ajax.php");
    CHECK(f.method == "post");

    std::string body = buildFormBody(f, "STN-26LFTBA055", "2272");
    CHECK(body.find("existing_userId=STN-26LFTBA055") != std::string::npos);
    CHECK(body.find("pin=2272") != std::string::npos);
    CHECK(body.find("error_message_incorrect_userid_password=") == std::string::npos);
}

int main() {
    test_generic_user_pass();
    test_email_field();
    test_default_text_type();
    test_mikrotik_chap_automated();
    test_mikrotik_chap_incomplete_fallback();
    test_stray_chap_id_is_plain_form();
    test_md5_chap_vectors();
    test_no_form_fallback();
    test_attribute_order();
    test_captive_detection();
    test_issue42_mikrotik_chap_login();
    test_issue44_mikrotik_default_template_chap_js();
    test_redirect_page_meta_refresh();
    test_redirect_page_js_location();
    test_redirect_js_bare_window_location();
    test_redirect_ignores_querystring_location();
    test_redirect_ignores_non_redirect_location_href_token();
    test_redirect_page_continue_link();
    test_redirect_ignores_generic_log_in_anchor();
    test_multi_form_picks_login_form();
    test_js_chap_undecodable_is_challenge();
    test_issue46_mikrotik_redirect_form_post();
    test_issue46_mikrotik_redirect_form_get();
    test_issue46_meta_refresh_beats_redirect_form();
    test_issue48_spectra_https_redirect_post();
    test_issue53_spectra_landing_form_full_hidden_echo();
    test_issue50_spectra_ajax_pin_login();
    test_issue55_spectra_ignores_hidden_error_text_inputs();

    if (g_failures == 0) {
        std::printf("\nAll captive-portal parser tests passed.\n");
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
