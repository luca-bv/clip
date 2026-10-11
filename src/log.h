#pragma once
#include <format>
#include <string>

void LogLine(const std::string& line);
template <class... A>
void Log(std::format_string<A...> f, A&&... a) {
  LogLine(std::format(f, std::forward<A>(a)...));
}
