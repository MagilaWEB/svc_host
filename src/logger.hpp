#pragma once

#include <windows.h>
#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>

enum class LogLevel
{
	Debug,
	Info,
	Warning,
	Error
};

class Logger
{
private:
	void Log(LogLevel level, std::wstring_view message);

	template<typename... Args>
	std::wstring msg(std::wstring_view message, Args&&... args)
	{
		std::lock_guard lock(m_mutex);

		return std::vformat(message, std::make_wformat_args(args...));
	}

public:
	static Logger& get();

	void SetLogLevel(LogLevel level);
	void SetLogFile(const std::wstring& path);

	template<typename... Args>
	void debug(std::wstring_view message, Args&&... args)
	{
		Log(LogLevel::Debug, msg(message, args...));
	}

	template<typename... Args>
	void info(std::wstring_view message, Args&&... args)
	{
		Log(LogLevel::Info, msg(message, args...));
	}

	template<typename... Args>
	void warning(std::wstring_view message, Args&&... args)
	{
		Log(LogLevel::Warning, msg(message, args...));
	}

	template<typename... Args>
	void error(std::wstring_view message, Args&&... args)
	{
		Log(LogLevel::Error, msg(message, args...));
	}

private:
	Logger();
	~Logger()						 = default;
	Logger(const Logger&)			 = delete;
	Logger& operator=(const Logger&) = delete;

	std::wstring GetTimestamp() const;
	std::wstring LevelToString(LogLevel level) const;

	LogLevel	   m_level = LogLevel::Info;
	std::wstring   m_logFilePath;
	std::wofstream m_fileStream;
	std::mutex	   m_mutex;
};
