#include "../native/csv.hpp"
#include <iostream>
#include <vector>

int main() {
  int checks = 0, failed = 0;
  const auto check = [&](bool result, const char* name) { ++checks; if (!result) { ++failed; std::cerr << name << '\n'; } };
  const auto field = [](std::string_view value) { taskmgr::Csv csv; csv.field(value); csv.endRow(); return csv.take(); };
  const std::string bom = "\xef\xbb\xbf";
  check(field("") == bom + "\"\"\r\n", "empty field");
  check(field("normal.exe") == bom + "\"normal.exe\"\r\n", "ordinary text");
  check(field("1,234") == bom + "\"1,234\"\r\n", "embedded delimiter");
  check(field("a\"b\"c") == bom + "\"a\"\"b\"\"c\"\r\n", "embedded quotes");
  check(field("line one\r\nline two") == bom + "\"line one\r\nline two\"\r\n", "embedded newline");
  check(field("Caf\xc3\xa9 \xf0\x9f\x9a\x80.exe") == bom + "\"Caf\xc3\xa9 \xf0\x9f\x9a\x80.exe\"\r\n", "UTF-8 remains intact");
  for (const auto value : {"=1+1", "+SUM(A1)", "-1+2", "@SUM(A1)", " \t=1+1", "\ttext", "\rtext", "\ntext", "\xef\xbc\x9d" "1+1", "\xef\xbc\x8b" "1+1", "\xef\xbc\x8d" "1+1", "\xef\xbc\xa0" "SUM(A1)"}) {
    check(field(value) == bom + "\"'" + value + "\"\r\n", "formula-like field remains text");
  }
  check(field(" process-name + suffix") == bom + "\" process-name + suffix\"\r\n", "internal operators are literal");
  check(field("   ") == bom + "\"   \"\r\n", "whitespace preserved");
  check(field(std::string("\0=1+1", 5)) == bom + "\"'\xef\xbf\xbd=1+1\"\r\n", "NUL replaced without hiding formula");
  taskmgr::Csv table; table.field("Name"); table.field("PID"); table.endRow(); table.field("first.exe"); table.field("42"); table.endRow(); table.field("second.exe"); table.field(""); table.endRow();
  const auto output = table.take();
  check(output == bom + "\"Name\",\"PID\"\r\n\"first.exe\",\"42\"\r\n\"second.exe\",\"\"\r\n", "multiple rows keep column boundaries");
  check(output.find(bom, bom.size()) == output.npos, "one encoding marker per file");
  std::cout << checks << " CSV checks, " << failed << " failed\n";
  return failed ? 1 : 0;
}
