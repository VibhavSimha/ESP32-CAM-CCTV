#include "captive_portal_parse.h"

#include <cctype>
#include <cstddef>

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

bool parseLoginForm(const std::string& html, PortalForm& out) {
    out = PortalForm();

    size_t formStart = ifind(html, "<form");
    if (formStart == std::string::npos) {
        return false; // No form at all -> browser-assisted fallback.
    }
    out.formFound = true;

    size_t openEnd = html.find('>', formStart);
    if (openEnd == std::string::npos) return false;
    std::string formTag = html.substr(formStart, openEnd - formStart);

    out.action = getAttr(formTag, "action");
    std::string method = toLowerCopy(getAttr(formTag, "method"));
    out.method = method.empty() ? "get" : method; // HTML default is GET.

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

        // A challenge/response portal (e.g. MikroTik CHAP md5) carries hidden
        // chap-id / chap-challenge fields and hashes the password in JS. Flag it
        // so the caller falls back to the manual browser flow.
        if (toLowerCopy(name).find("chap") != std::string::npos) {
            out.challenge = true;
        }

        if (name.empty()) continue; // Cannot submit a nameless field.

        if (type == "password") {
            if (out.passField.empty()) out.passField = name;
        } else if (type == "hidden") {
            out.hidden.push_back(PortalFormField{name, value});
        } else if (type == "submit" || type == "button" || type == "reset" ||
                   type == "checkbox" || type == "radio" || type == "image" ||
                   type == "file") {
            // Not a credential field; ignore for auto-detection.
        } else if (isTextLikeType(type)) {
            if (out.userField.empty()) out.userField = name;
        }
    }

    out.valid = !out.challenge && !out.userField.empty() && !out.passField.empty();
    return out.valid;
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

std::string buildFormBody(const PortalForm& form,
                          const std::string& username,
                          const std::string& password) {
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
        body += urlEncode(form.passField) + "=" + urlEncode(password);
    }
    return body;
}
