#include "captive_portal_parse.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

// -----------------------------------------------------------------------------
// Small string helpers (case-insensitive), intentionally dependency-free so the
// parser compiles both in the ESP32 firmware and in the host unit test.
// -----------------------------------------------------------------------------

static std::string toLowerCopy(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Case-insensitive equality for a single char.
static inline bool ciEqualChar(char a, char b) {
    return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
}

// Case-insensitive search for `needle` in `hay` starting at `from`. Allocation-
// free (does not copy `hay`), which matters when scanning a multi-KB portal page
// on a heap-constrained ESP32.
static size_t ifind(const std::string& hay, const std::string& needle, size_t from = 0) {
    if (needle.empty()) return from <= hay.size() ? from : std::string::npos;
    if (from >= hay.size()) return std::string::npos;
    const size_t n = needle.size();
    if (hay.size() < n) return std::string::npos;
    for (size_t i = from; i + n <= hay.size(); i++) {
        size_t j = 0;
        for (; j < n; j++) {
            if (!ciEqualChar(hay[i + j], needle[j])) break;
        }
        if (j == n) return i;
    }
    return std::string::npos;
}

// Extract the value of attribute `attr` from a single tag's text (e.g. the text
// between `<input` and `>`). Supports double quotes, single quotes and unquoted
// values. Returns "" when the attribute is absent (value-less attributes such as
// a bare `disabled` yield "").
static std::string getAttr(const std::string& tag, const std::string& attr) {
    const std::string lower = toLowerCopy(tag);
    const std::string key = toLowerCopy(attr);
    size_t pos = 0;
    while ((pos = lower.find(key, pos)) != std::string::npos) {
        // Ensure the match is a real attribute boundary (preceded by whitespace
        // or the tag start) so "name" does not match inside "formname".
        bool boundaryOk = (pos == 0) || std::isspace((unsigned char)lower[pos - 1]);
        size_t after = pos + key.size();
        // Skip trailing whitespace before '='.
        size_t eq = after;
        while (eq < lower.size() && std::isspace((unsigned char)lower[eq])) eq++;
        if (!boundaryOk || eq >= lower.size() || lower[eq] != '=') {
            pos = after;
            continue;
        }
        size_t v = eq + 1;
        while (v < tag.size() && std::isspace((unsigned char)tag[v])) v++;
        if (v >= tag.size()) return "";
        char q = tag[v];
        if (q == '"' || q == '\'') {
            size_t end = tag.find(q, v + 1);
            if (end == std::string::npos) return tag.substr(v + 1);
            return tag.substr(v + 1, end - (v + 1));
        }
        // Unquoted value: read until whitespace or tag end.
        size_t end = v;
        while (end < tag.size() && !std::isspace((unsigned char)tag[end]) &&
               tag[end] != '>' && tag[end] != '/') {
            end++;
        }
        return tag.substr(v, end - v);
    }
    return "";
}

// Return true if `type` is a text-like input we can treat as the username field.
static bool isTextLikeType(const std::string& type) {
    if (type.empty()) return true; // HTML default input type is "text".
    return type == "text" || type == "email" || type == "tel" ||
           type == "number" || type == "search" || type == "url";
}

// Decode an even-length hex string (e.g. a MikroTik chap-challenge) into raw
// bytes. Returns false when `s` is empty, has an odd length, or contains a
// non-hex digit — in which case the caller must NOT attempt an automated CHAP
// login (a wrong hash is worse than falling back to the manual browser flow).
static bool hexDecode(const std::string& s, std::string& out) {
    if (s.empty() || (s.size() % 2) != 0) return false;
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nibble(s[i]);
        int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        decoded.push_back((char)((hi << 4) | lo));
    }
    out.swap(decoded);
    return true;
}

// Parse ONE <form> whose "<form" begins at `formStart`. Fills `f` (action,
// method, userField, passField, hidden inputs) and reports any chap-id /
// chap-challenge carried as hidden <input> fields (issue #42) via `chapIdRaw` /
// `chapChallengeRaw`. Returns the index just past this form's "</form>" so the
// caller can keep scanning for further forms on the page.
static size_t parseSingleForm(const std::string& html, size_t formStart,
                              PortalForm& f, std::string& chapIdRaw,
                              std::string& chapChallengeRaw) {
    f = PortalForm();
    f.formFound = true;
    chapIdRaw.clear();
    chapChallengeRaw.clear();

    size_t openEnd = html.find('>', formStart);
    if (openEnd == std::string::npos) return std::string::npos;
    std::string formTag = html.substr(formStart, openEnd - formStart);

    f.action = getAttr(formTag, "action");
    std::string method = toLowerCopy(getAttr(formTag, "method"));
    f.method = method.empty() ? "get" : method; // HTML default is GET.

    size_t formEnd = ifind(html, "</form>", openEnd);
    size_t bodyEnd = (formEnd == std::string::npos) ? html.size() : formEnd;

    // Walk every <input> tag inside the form body.
    size_t pos = openEnd + 1;
    while (pos < bodyEnd) {
        size_t in = ifind(html, "<input", pos);
        if (in == std::string::npos || in >= bodyEnd) break;
        size_t tagEnd = html.find('>', in);
        if (tagEnd == std::string::npos || tagEnd > bodyEnd) tagEnd = bodyEnd;
        std::string tag = html.substr(in, tagEnd - in);
        pos = tagEnd + 1;

        std::string type = toLowerCopy(getAttr(tag, "type"));
        std::string name = getAttr(tag, "name");
        std::string value = getAttr(tag, "value");

        // A MikroTik hotspot CHAP portal may carry hidden chap-id / chap-challenge
        // fields and expects password = MD5(chap-id + password + chap-challenge).
        // Capture the values so we can compute that hash ourselves and log the
        // user in automatically instead of falling back to the browser (issue #42).
        std::string lname = toLowerCopy(name);
        if (lname == "chap-id") chapIdRaw = value;
        else if (lname == "chap-challenge") chapChallengeRaw = value;

        if (name.empty()) continue; // Cannot submit a nameless field.

        if (type == "password") {
            if (f.passField.empty()) f.passField = name;
        } else if (type == "hidden") {
            f.hidden.push_back(PortalFormField{name, value});
        } else if (type == "submit" || type == "button" || type == "reset" ||
                   type == "checkbox" || type == "radio" || type == "image" ||
                   type == "file") {
            // Not a credential field; ignore for auto-detection.
        } else if (isTextLikeType(type)) {
            if (f.userField.empty()) f.userField = name;
        }
    }

    if (formEnd == std::string::npos) return html.size();
    return formEnd + 7; // strlen("</form>")
}

// Skip ASCII whitespace from `i`.
static size_t skipWs(const std::string& s, size_t i) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    return i;
}

// Parse a quoted JS string literal that starts at/after `from` (after optional
// whitespace). Returns "" when no quoted literal begins there.
static std::string quotedLiteralFrom(const std::string& html, size_t from) {
    size_t i = skipWs(html, from);
    if (i >= html.size()) return "";
    char q = html[i];
    if (q != '\'' && q != '"') return "";
    size_t end = html.find(q, i + 1);
    if (end == std::string::npos) return "";
    return html.substr(i + 1, end - (i + 1));
}

// Extract URL from a JavaScript assignment such as:
//   <lhs> = "..."
// Example keys: "window.location", "location.href".
static std::string extractJsAssignUrl(const std::string& html, const char* lhs) {
    size_t pos = 0;
    while ((pos = ifind(html, lhs, pos)) != std::string::npos) {
        size_t i = skipWs(html, pos + std::strlen(lhs));
        if (i < html.size() && html[i] == '.') {
            // "window.location" is a prefix of ".href/.replace/..." forms;
            // do not treat those as bare assignment matches here.
            pos++;
            continue;
        }
        if (i >= html.size() || html[i] != '=') {
            pos++;
            continue;
        }
        std::string url = quotedLiteralFrom(html, i + 1);
        if (!url.empty()) return url;
        pos++;
    }
    return "";
}

// Extract URL from a JavaScript call such as:
//   <callee>("...")
// Example keys: "location.replace", "location.assign".
static std::string extractJsCallUrl(const std::string& html, const char* callee) {
    size_t pos = 0;
    while ((pos = ifind(html, callee, pos)) != std::string::npos) {
        size_t i = skipWs(html, pos + std::strlen(callee));
        if (i >= html.size() || html[i] != '(') {
            pos++;
            continue;
        }
        std::string url = quotedLiteralFrom(html, i + 1);
        if (!url.empty()) return url;
        pos++;
    }
    return "";
}

// MikroTik hotspot login pages compute the CHAP response in JavaScript, inlining
// the per-session tokens as quoted hex literals:
//   hexMD5('<chap-id>' + document.login.password.value + '<chap-challenge>')
// Pull the first and last quoted literals out of that call so a page whose CHAP
// tokens live ONLY in md5.js (not as hidden <input>s) is still automatable — this
// is the default MikroTik template behind issue #44. Returns false when no such
// call (with two quoted literals) is present.
static bool extractJsChap(const std::string& html, std::string& idHex,
                          std::string& challengeHex) {
    size_t call = ifind(html, "hexmd5(");
    if (call == std::string::npos) return false;
    size_t open = html.find('(', call);
    if (open == std::string::npos) return false;
    size_t close = html.find(')', open);
    if (close == std::string::npos) return false;

    std::vector<std::string> literals;
    size_t i = open + 1;
    while (i < close) {
        char c = html[i];
        if (c == '\'' || c == '"') {
            size_t end = html.find(c, i + 1);
            if (end == std::string::npos || end > close) break;
            literals.push_back(html.substr(i + 1, end - (i + 1)));
            i = end + 1;
        } else {
            i++;
        }
    }
    if (literals.size() < 2) return false;
    idHex = literals.front();
    challengeHex = literals.back();
    return !idHex.empty() && !challengeHex.empty();
}

// A landing/redirect page (no usable login form) still tells the browser where
// the real login page is. Extract that next-hop URL, preferring the most reliable
// signal: a <meta http-equiv="refresh" content="N; url=…">, then a JS
// window.location redirect, then a "continue"/login anchor. Returns "" when the
// page carries no redirect hint. The caller resolves relative URLs against the
// page URL and follows ONE hop (issue #44).
static std::string extractRedirectUrl(const std::string& html) {
    // 1. <meta http-equiv="refresh" content="0; url=http://…">
    size_t m = 0;
    while ((m = ifind(html, "<meta", m)) != std::string::npos) {
        size_t end = html.find('>', m);
        if (end == std::string::npos) break;
        std::string tag = html.substr(m, end - m);
        m = end + 1;
        if (toLowerCopy(getAttr(tag, "http-equiv")) != "refresh") continue;
        std::string content = getAttr(tag, "content");
        size_t u = toLowerCopy(content).find("url=");
        if (u == std::string::npos) continue;
        std::string url = content.substr(u + 4);
        // Trim surrounding whitespace and optional quotes.
        while (!url.empty() && std::isspace((unsigned char)url.front())) url.erase(url.begin());
        while (!url.empty() && (url.back() == '\'' || url.back() == '"' ||
                                std::isspace((unsigned char)url.back()))) url.pop_back();
        if (!url.empty() && (url.front() == '\'' || url.front() == '"')) url.erase(url.begin());
        if (!url.empty()) return url;
    }

    // 2. JavaScript redirects: window.location.href='…', location.replace('…'),
    //    location.assign('…'), window.location='…'. Match only real assignment/
    //    call syntax (not a random "location.href" token in JSON/content), else
    //    unrelated pages can be misread as redirects.
    std::string jsUrl = extractJsAssignUrl(html, "window.location.href");
    if (!jsUrl.empty()) return jsUrl;
    jsUrl = extractJsAssignUrl(html, "location.href");
    if (!jsUrl.empty()) return jsUrl;
    jsUrl = extractJsCallUrl(html, "location.replace");
    if (!jsUrl.empty()) return jsUrl;
    jsUrl = extractJsCallUrl(html, "location.assign");
    if (!jsUrl.empty()) return jsUrl;
    jsUrl = extractJsAssignUrl(html, "window.location");
    if (!jsUrl.empty()) return jsUrl;

    // 3. A "continue"/login anchor: <a href="…">…continue…</a>.
    std::string htmlLower = toLowerCopy(html);
    bool hasRedirectCue = htmlLower.find("if you are not redirected") != std::string::npos ||
                          htmlLower.find("redirected in a few seconds") != std::string::npos ||
                          htmlLower.find("click continue") != std::string::npos;
    size_t a = 0;
    while ((a = ifind(html, "<a", a)) != std::string::npos) {
        size_t end = html.find('>', a);
        if (end == std::string::npos) break;
        std::string tag = html.substr(a, end - a);
        size_t textEnd = ifind(html, "</a>", end);
        std::string text = (textEnd == std::string::npos)
                               ? std::string()
                               : toLowerCopy(html.substr(end + 1, textEnd - (end + 1)));
        a = end + 1;
        std::string href = getAttr(tag, "href");
        if (href.empty() || href == "#") continue;
        bool continueLike = text.find("continue") != std::string::npos;
        bool loginLike = text.find("login") != std::string::npos ||
                         text.find("log in") != std::string::npos;
        if (continueLike || (loginLike && hasRedirectCue)) {
            return href;
        }
    }
    return "";
}

bool parseLoginForm(const std::string& html, PortalForm& out) {
    out = PortalForm();

    // Scan EVERY <form> on the page rather than assuming the first one is the
    // login form. The MikroTik default login page, for example, emits a hidden
    // "sendin" form (chap plumbing, no visible fields) BEFORE the real username/
    // password form, so the first <form> alone yields "Form incomplete" (the
    // exact issue #44 failure).
    PortalForm best;                // first fully-usable username+password form
    bool haveBest = false;
    PortalForm challengeCandidate;  // a CHAP form we could not decode safely
    bool haveChallenge = false;
    bool anyForm = false;
    PortalForm continueForm;        // first "redirect"/"continue" landing form (no creds)
    bool haveContinueForm = false;

    size_t search = 0;
    while (true) {
        size_t formStart = ifind(html, "<form", search);
        if (formStart == std::string::npos) break;
        anyForm = true;

        PortalForm cand;
        std::string chapIdRaw, chapChallengeRaw;
        size_t next = parseSingleForm(html, formStart, cand, chapIdRaw, chapChallengeRaw);
        search = (next == std::string::npos || next <= formStart) ? (formStart + 5) : next;

        bool hasUserPass = !cand.userField.empty() && !cand.passField.empty();

        // A form carrying BOTH chap-id and chap-challenge as hidden inputs is a
        // MikroTik CHAP form (issue #42). Automate it only when both decode AND a
        // username/password field is present; otherwise never send the plaintext
        // password to a CHAP portal — remember it as a manual-fallback challenge.
        if (!chapIdRaw.empty() && !chapChallengeRaw.empty()) {
            std::string idBytes, challengeBytes;
            if (hasUserPass && hexDecode(chapIdRaw, idBytes) &&
                hexDecode(chapChallengeRaw, challengeBytes)) {
                cand.chapLogin = true;
                cand.chapId = idBytes;
                cand.chapChallenge = challengeBytes;
                cand.valid = true;
                out = cand;
                return true;
            }
            if (!haveChallenge) {
                challengeCandidate = cand;
                challengeCandidate.challenge = true;
                challengeCandidate.valid = false;
                haveChallenge = true;
            }
            continue;
        }

        if (hasUserPass) {
            if (!haveBest) { best = cand; haveBest = true; }
        } else if (!haveContinueForm && cand.userField.empty() &&
                   cand.passField.empty() && !cand.action.empty()) {
            // A form carrying NEITHER a username nor a password input, but WITH an
            // action, is a MikroTik-style "redirect"/"continue" landing form that
            // the browser auto-submits to reach the real login page. Remember the
            // first one (GET *or* POST) as the next-hop fallback; the previous code
            // kept only GET forms, so a POST redirect form — used by RADIUSdesk
            // external portals — was dropped and the flow dead-ended with
            // redirect:'' (issue #46).
            continueForm = cand;
            haveContinueForm = true;
        }
    }

    if (haveBest) {
        out = best;
        // A plain username+password form on a MikroTik-style page may still be a
        // CHAP login: the chap-id/chap-challenge live in the page's md5.js call,
        // not in <input>s. Detect that and hash locally (issue #44) rather than
        // sending the plaintext password to a CHAP portal.
        std::string idHex, challengeHex;
        if (extractJsChap(html, idHex, challengeHex)) {
            std::string idBytes, challengeBytes;
            if (hexDecode(idHex, idBytes) && hexDecode(challengeHex, challengeBytes)) {
                out.chapLogin = true;
                out.chapId = idBytes;
                out.chapChallenge = challengeBytes;
            } else {
                // The page is CHAP but the tokens will not decode — do NOT fall
                // back to submitting the plaintext password.
                out.valid = false;
                out.challenge = true;
                out.redirectUrl = extractRedirectUrl(html);
                return false;
            }
        }
        out.valid = true;
        return true;
    }

    if (haveChallenge) {
        out = challengeCandidate;
        out.redirectUrl = extractRedirectUrl(html);
        return false;
    }

    // Forms exist but none carry a username+password, OR there is no form at all
    // (a JS/redirect landing page). Offer a next-hop URL so the caller can follow
    // one redirect to the real login page and re-parse it (issue #44).
    out = PortalForm();
    out.formFound = anyForm;
    out.redirectUrl = extractRedirectUrl(html);
    if (out.redirectUrl.empty() && haveContinueForm) {
        // No <meta refresh> / JS location / "continue" anchor hint on the page —
        // fall back to the MikroTik-style "redirect"/"continue" landing FORM,
        // preserving its method and hidden fields so the caller resubmits it
        // exactly as the browser would (a POST form was previously dropped, which
        // dead-ended the flow with redirect:'' — issue #46).
        out.redirectUrl = continueForm.action;
        out.redirectMethod = continueForm.method;
        out.redirectFields = continueForm.hidden;
    }
    return false;
}

bool looksLikeCaptivePortal(int status, const std::string& body) {
    // A clean generate_204 probe returns 204 with an empty body. Anything else —
    // a redirect (3xx), a 200 with an HTML login page, or unexpected content —
    // means the network intercepted the request: a captive portal.
    if (status == 204 && body.empty()) return false;
    if (status >= 300 && status < 400) return true; // redirect to portal
    if (status == 200 && !body.empty()) return true; // portal served its page
    if (status == 204) return true; // 204 but with body -> not a clean pass
    return false;
}

// Minimal RFC3986-ish form-url-encoder for a single component.
static std::string urlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Self-contained MD5 (RFC 1321). MD5 is used ONLY because the MikroTik hotspot
// CHAP login protocol mandates it — the router expects
// password = hex(MD5(chap-id + password + chap-challenge)). It is NOT used for
// any security decision of our own. Kept dependency-free (no mbedTLS/Arduino) so
// the CHAP login runs identically in the firmware and in the host unit tests.
// -----------------------------------------------------------------------------
static inline uint32_t md5Rotl(uint32_t x, uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

static void md5Raw(const unsigned char* msg, size_t len, unsigned char digest[16]) {
    static const uint32_t s[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    // Pre-process: append 0x80, pad with zeros to 56 mod 64, then the 64-bit
    // little-endian bit length. Build the padded message explicitly so the code
    // is byte-order independent (works on any host, not just little-endian).
    const size_t paddedLen = ((len + 8) / 64 + 1) * 64;
    std::string buf(paddedLen, (char)0);
    std::memcpy(&buf[0], msg, len);
    buf[len] = (char)0x80;
    uint64_t bitLen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        buf[paddedLen - 8 + i] = (char)((bitLen >> (8 * i)) & 0xFF);
    }

    const unsigned char* p = (const unsigned char*)buf.data();
    for (size_t off = 0; off < paddedLen; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            M[i] = (uint32_t)p[off + i * 4] |
                   ((uint32_t)p[off + i * 4 + 1] << 8) |
                   ((uint32_t)p[off + i * 4 + 2] << 16) |
                   ((uint32_t)p[off + i * 4 + 3] << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) {
                F = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            F = F + A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B = B + md5Rotl(F, s[i]);
        }
        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    }

    const uint32_t words[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++) {
        digest[i * 4]     = (unsigned char)(words[i] & 0xFF);
        digest[i * 4 + 1] = (unsigned char)((words[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = (unsigned char)((words[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = (unsigned char)((words[i] >> 24) & 0xFF);
    }
}

std::string mikrotikChapPassword(const std::string& chapId,
                                 const std::string& chapChallenge,
                                 const std::string& password) {
    std::string msg;
    msg.reserve(chapId.size() + password.size() + chapChallenge.size());
    msg += chapId;
    msg += password;
    msg += chapChallenge;
    unsigned char digest[16];
    md5Raw((const unsigned char*)msg.data(), msg.size(), digest);
    static const char hx[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; i++) {
        out.push_back(hx[(digest[i] >> 4) & 0xF]);
        out.push_back(hx[digest[i] & 0xF]);
    }
    return out;
}

std::string buildFormBody(const PortalForm& form,
                          const std::string& username,
                          const std::string& password) {
    // For a MikroTik CHAP portal the router expects the MD5 challenge response,
    // never the plaintext password (issue #42).
    std::string effectivePassword = form.chapLogin
        ? mikrotikChapPassword(form.chapId, form.chapChallenge, password)
        : password;

    std::string body;
    for (const PortalFormField& f : form.hidden) {
        if (!body.empty()) body += "&";
        body += urlEncode(f.name) + "=" + urlEncode(f.value);
    }
    if (!form.userField.empty()) {
        if (!body.empty()) body += "&";
        body += urlEncode(form.userField) + "=" + urlEncode(username);
    }
    if (!form.passField.empty()) {
        if (!body.empty()) body += "&";
        body += urlEncode(form.passField) + "=" + urlEncode(effectivePassword);
    }
    return body;
}
