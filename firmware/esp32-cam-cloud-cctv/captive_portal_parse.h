#pragma once

// =============================================================================
//  captive_portal_parse.h — pure C++ (no Arduino) HTML login-form auto-detection
//
//  This is the crux of the browser-assisted captive-portal feature (issue #33):
//  we DO NOT hardcode the ISP portal's field names. Instead we parse the portal
//  login page and auto-detect the two inputs that matter — the first text-like
//  input (username) and the password input — plus any hidden fields we must
//  echo back on submit.
//
//  Kept free of Arduino types (uses std::string/std::vector) so the detection
//  logic can be unit-tested on the host with a normal C++ compiler. See
//  test/test_captive_portal_parse.cpp.
// =============================================================================

#include <string>
#include <vector>

struct PortalFormField {
    std::string name;
    std::string value;
};

struct PortalForm {
    bool formFound = false;        // a <form> ... </form> block was located
    bool valid = false;            // usable for automated ESP32-side submit
    bool challenge = false;        // challenge/response portal we CANNOT automate
    bool chapLogin = false;        // MikroTik CHAP portal we CAN automate (issue #42)
    std::string action;            // form action (may be empty -> post to page URL)
    std::string method;            // "post" or "get" (lowercase)
    std::string userField;         // auto-detected username input name
    std::string passField;         // auto-detected password input name
    std::string chapId;            // decoded raw bytes of chap-id (CHAP only)
    std::string chapChallenge;     // decoded raw bytes of chap-challenge (CHAP only)
    std::string deviceTypeField;   // optional device-name field used by some portals
    std::vector<PortalFormField> hidden; // hidden inputs to echo back on submit
    // A (possibly relative) URL to follow when THIS page carries no usable login
    // form but is a redirect/landing page — e.g. a MikroTik hotspot serves an
    // rlogin-style page ("If you are not redirected… click continue") that only
    // reaches the real login.html via a <meta refresh>, a JS location redirect,
    // or a "continue" link/form. The caller follows one hop and re-parses so the
    // ESP32 can reach — and auto-submit — the real login form (issue #44).
    std::string redirectUrl;
    // How to follow `redirectUrl`. Empty (or "get") means a plain GET navigation
    // (a <meta refresh>, JS location change or "continue" link). "post" means the
    // next hop is a MikroTik-style "redirect"/"continue" <form method="post"> that
    // the browser AUTO-SUBMITS to reach the real login page — common with
    // RADIUSdesk external portals. The caller must resubmit it as a POST, echoing
    // `redirectFields`, or it dead-ends on the landing page with redirect:'' (the
    // exact issue #46 failure).
    std::string redirectMethod;
    std::vector<PortalFormField> redirectFields;
};

// Auto-detect the login form in `html`. Returns true when a form simple enough
// to automate is found — either a plain username+password form, or a MikroTik
// hotspot CHAP form whose password we can hash ourselves (issue #42).
//
// The whole page is scanned: a portal page can carry SEVERAL forms (the MikroTik
// default login page has a hidden "sendin" form BEFORE the visible login form),
// so the first <form> is not assumed to be the login form. MikroTik pages that
// carry chap-id/chap-challenge only inside their md5.js call (not as <input>s)
// are detected too (issue #44).
//
// On a challenge / JS-only / no-form / redirect-only page it returns false and
// sets flags (and `out.redirectUrl` when a next-hop URL is present) so the caller
// can follow a redirect and/or fall back to the browser-assisted manual path.
bool parseLoginForm(const std::string& html, PortalForm& out);

// Heuristic: does an HTTP connectivity-probe response look like a captive portal
// interception? `status` is the HTTP status code of a request that SHOULD have
// returned 204 with an empty body (e.g. connectivitycheck generate_204).
bool looksLikeCaptivePortal(int status, const std::string& body);

// Build an application/x-www-form-urlencoded body from the detected form,
// substituting the user-supplied username/password into the auto-detected
// fields and echoing every hidden field. For a MikroTik CHAP form (issue #42)
// the password is replaced by the CHAP response hash — the plaintext password is
// never sent. Exposed for host testing.
std::string buildFormBody(const PortalForm& form,
                          const std::string& username,
                          const std::string& password,
                          const std::string& deviceType = std::string());

// Compute the MikroTik hotspot CHAP login response:
//   hex( MD5( chapId + password + chapChallenge ) )
// where `chapId` and `chapChallenge` are the RAW (already hex-decoded) byte
// strings taken from the portal page. MD5 here is mandated by the MikroTik CHAP
// hotspot protocol; it is not used for any security decision of our own. Exposed
// for host testing against known vectors.
std::string mikrotikChapPassword(const std::string& chapId,
                                 const std::string& chapChallenge,
                                 const std::string& password);
