#pragma once

#include <windows.h>
#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

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
		std::lock_guard lock(_mutex);

		return std::vformat(message, std::make_wformat_args(args...));
	}

public:
	static Logger& get();

	void setLogLevel(LogLevel level);
	void setLogFile(std::filesystem::path path);

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

	void _logOpen();

	std::wstring GetTimestamp() const;
	std::wstring LevelToString(LogLevel level) const;

	LogLevel			  _level = LogLevel::Info;
	std::filesystem::path _log_file_path;
	std::wofstream		  _file_stream;
	std::mutex			  _mutex;
};
