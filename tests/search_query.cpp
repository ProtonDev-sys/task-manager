#include "../native/search_query.hpp"
#include <iostream>

int main() {
  using namespace taskmgr;
  int failures = 0, checks = 0;
  const auto check = [&](bool result, const char* name) { ++checks; if (!result) { std::cerr << name << '\n'; ++failures; } };
  const auto plain = [&](std::wstring_view text) {
    const auto query = parseSearchQuery(text);
    return !query.scoped && (text.empty() ? query.terms.empty() : query.terms.size() == 1 && query.terms[0].field == SearchField::Any && query.terms[0].text == text && !query.terms[0].excluded);
  };
  check(plain(L""), "empty query");
  check(plain(L"Orbit Browser"), "plain phrase keeps spaces");
  check(plain(L"  Orbit Browser  "), "plain phrase keeps surrounding spaces");
  check(plain(L"\"Orbit Browser\""), "plain phrase keeps quotes");
  check(plain(L"C:\\Program Files\\Orbit\\browser.exe"), "Windows path remains literal");
  check(plain(L"--profile \"Main User\" -private"), "command switches remain literal");
  check(plain(L"https://example.test/path"), "URL remains literal");
  check(plain(L"unknown:value -other:value"), "unknown prefixes remain literal");
  check(plain(L"\"name:literal pid:42\""), "quoted fields do not enable syntax");
  check(plain(L"42"), "plain PID retains substring mode");
  check(plain(L"caf\u00e9 \u5de5\u5177"), "Unicode phrase preserved");
  {
    const auto query = parseSearchQuery(L"name:browser pid:4242 publisher:\"Orbit Labs\" -status:suspended");
    check(query.scoped && query.terms.size() == 4, "all scoped terms parsed");
    if (query.terms.size() == 4) {
      check(query.terms[0].field == SearchField::Name && query.terms[0].text == L"browser" && !query.terms[0].excluded, "name term");
      check(query.terms[1].field == SearchField::Pid && query.terms[1].pid == 4242 && query.terms[1].numeric, "exact PID term");
      check(query.terms[2].field == SearchField::Publisher && query.terms[2].text == L"Orbit Labs", "quoted publisher");
      check(query.terms[3].field == SearchField::Status && query.terms[3].text == L"suspended" && query.terms[3].excluded, "excluded status");
    }
  }
  {
    const auto query = parseSearchQuery(L"PATH:\"C:\\Program Files\\Orbit\\\" User:DOMAIN\\Alice command:\"--profile Main\" \"quoted words\" --flag unknown:value");
    check(query.scoped && query.terms.size() == 6, "case insensitive fields and ordinary terms");
    if (query.terms.size() == 6) {
      check(query.terms[0].field == SearchField::Path && query.terms[0].text == L"C:\\Program Files\\Orbit\\", "quoted Windows path keeps trailing backslash");
      check(query.terms[1].field == SearchField::User && query.terms[1].text == L"DOMAIN\\Alice", "account separator preserved");
      check(query.terms[2].field == SearchField::Command && query.terms[2].text == L"--profile Main", "quoted command switches");
      check(query.terms[3].field == SearchField::Any && query.terms[3].text == L"quoted words", "unqualified phrase in scoped query");
      check(query.terms[4].field == SearchField::Any && query.terms[4].text == L"--flag" && !query.terms[4].excluded, "unqualified switches are not exclusions");
      check(query.terms[5].field == SearchField::Any && query.terms[5].text == L"unknown:value", "unknown scoped prefix remains literal");
    }
  }
  {
    const auto query = parseSearchQuery(L"-name:browser");
    check(query.scoped && query.terms.size() == 1 && query.terms[0].excluded && query.terms[0].field == SearchField::Name, "exclusion only query");
  }
  {
    const auto query = parseSearchQuery(L"name:\"unfinished phrase");
    check(query.scoped && query.terms.size() == 1 && query.terms[0].text == L"unfinished phrase", "incomplete quote remains editable");
  }
  {
    const auto query = parseSearchQuery(L"pid: name:\"\"");
    check(query.scoped && query.terms.size() == 2 && query.terms[0].text.empty() && !query.terms[0].pid && query.terms[1].text.empty(), "incomplete field does not become wildcard");
  }
  {
    const auto query = parseSearchQuery(L"name:browser\tstatus:running\r\npid:42");
    check(query.scoped && query.terms.size() == 3 && query.terms[2].pid == 42, "ASCII whitespace splits scoped terms");
  }
  for (const auto& [text, expected] : std::vector<std::pair<std::wstring, uint32_t>>{{L"0", 0}, {L"00042", 42}, {L"4294967295", 4294967295u}}) {
    check(searchPid(text) == expected, "valid uint32 PID");
    const auto query = parseSearchQuery(L"pid:" + text);
    check(query.scoped && query.terms.size() == 1 && query.terms[0].pid == expected, "parsed exact PID");
  }
  for (const std::wstring_view text : {L"", L"-1", L"+1", L" 42", L"42 ", L"42x", L"1.0", L"4294967296", L"999999999999999999999999", L"\uff11"}) check(!searchPid(text), "invalid or overflowing PID rejected");
  check(parseSearchQuery(L"pid:4294967296").terms[0].pid == std::nullopt, "overflowing scoped PID is not a substring");
  std::cout << checks << " search query checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
