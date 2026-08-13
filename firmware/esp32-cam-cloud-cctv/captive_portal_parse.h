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
    bool challenge = false;        // challenge/response portal (e.g. MikroTik CHAP)
    std::string action;            // form action (may be empty -> post to page URL)
    std::string method;            // "post" or "get" (lowercase)
    std::string userField;         // auto-detected username input name
    std::string passField;         // auto-detected password input name
    std::vector<PortalFormField> hidden; // hidden inputs to echo back on submit
};

// Auto-detect the login form in `html`. Returns true when the form is simple
// enough to automate (a username field, a password field, no challenge). On a
// challenge/JS-only/no-form page, returns false and sets flags so the caller can
// fall back to the browser-assisted manual path.
bool parseLoginForm(const std::string& html, PortalForm& out);

// Heuristic: does an HTTP connectivity-probe response look like a captive portal
// interception? `status` is the HTTP status code of a request that SHOULD have
// returned 204 with an empty body (e.g. connectivitycheck generate_204).
bool looksLikeCaptivePortal(int status, const std::string& body);

// Build an application/x-www-form-urlencoded body from the detected form,
// substituting the user-supplied username/password into the auto-detected
// fields and echoing every hidden field. Exposed for host testing.
std::string buildFormBody(const PortalForm& form,
                          const std::string& username,
                          const std::string& password);
