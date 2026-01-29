#pragma once

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#if __has_include(<spdlog/fmt/bundled/format.h>)
#include <spdlog/fmt/bundled/format.h>
#elif __has_include(<spdlog/fmt/fmt.h>)
#include <spdlog/fmt/fmt.h>
#else
#include <fmt/format.h>
#endif

// fmt 11.x no longer auto-formats enums and std::atomic.
// Provide generic formatters for compatibility.
#if FMT_VERSION >= 110000
#include <type_traits>
#include <atomic>

template <typename T>
struct fmt::formatter<T, char, std::enable_if_t<std::is_enum_v<T>>> : fmt::formatter<std::underlying_type_t<T>> {
    auto format(T e, fmt::format_context& ctx) const {
        return fmt::formatter<std::underlying_type_t<T>>::format(
            static_cast<std::underlying_type_t<T>>(e), ctx);
    }
};

template <typename T>
struct fmt::formatter<std::atomic<T>, char, std::enable_if_t<std::is_arithmetic_v<T>>> : fmt::formatter<T> {
    auto format(const std::atomic<T>& a, fmt::format_context& ctx) const {
        return fmt::formatter<T>::format(a.load(), ctx);
    }
};
#endif

namespace fmtutil
{
	template<typename... Args>
	inline char* format_to(char* buffer, const char* format, const Args& ...args) noexcept
	{
		char* s = fmt::format_to(buffer, format, args...);
		s[0] = '\0';
		return s;
	}

	template<int BUFSIZE=512, typename... Args>
	inline const char* format(const char* format, const Args& ...args) noexcept
	{
		thread_local static char buffer[BUFSIZE];
		char* s = fmt::format_to(buffer, format, args...);
		s[0] = '\0';
		return buffer;
	}
}
