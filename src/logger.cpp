#include "logger.hpp"
#include <filesystem>

Logger& Logger::get()
{
	static Logger instance;
	return instance;
}

Logger::Logger()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::filesystem::path exePath(path);
	_log_file_path = (exePath.parent_path() / L"SvcHost.log").wstring();
}

void Logger::setLogLevel(LogLevel level)
{
	std::lock_guard lock(_mutex);
	_level = level;
}

void Logger::setLogFile(std::filesystem::path path)
{
	std::lock_guard lock(_mutex);

	if (_file_stream.is_open())
		_file_stream.close();

	_log_file_path = path;
}

void Logger::Log(LogLevel level, std::wstring_view message)
{
	if (level < _level)
		return;

	_logOpen();

	std::wstring formatted = GetTimestamp() + L" [" + LevelToString(level) + L"] " + std::wstring(message);

	{
		std::lock_guard lock(_mutex);
		if (_file_stream.is_open())
		{
			_file_stream << formatted << std::endl;
			_file_stream.flush();
		}
	}

	// IDE
	OutputDebugStringW((formatted + L"\n").c_str());
}

void Logger::_logOpen()
{
	if (_file_stream.is_open())
		return;

	_file_stream.open(_log_file_path, std::ios::out | std::ios::trunc);

	if (!_file_stream.is_open())
		_file_stream.open(_log_file_path, std::ios::out | std::ios::app);
}

std::wstring Logger::GetTimestamp() const
{
	auto	now	   = std::chrono::system_clock::now();
	auto	time_t = std::chrono::system_clock::to_time_t(now);
	std::tm tm;

	localtime_s(&tm, &time_t);

	std::wostringstream woss;
	woss << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S");

	return woss.str();
}

std::wstring Logger::LevelToString(LogLevel level) const
{
	switch (level)
	{
	case LogLevel::Debug:
		return L"DEBUG";
	case LogLevel::Info:
		return L"INFO";
	case LogLevel::Warning:
		return L"WARN";
	case LogLevel::Error:
		return L"ERROR";
	default:
		return L"UNKNOWN";
	}
}
