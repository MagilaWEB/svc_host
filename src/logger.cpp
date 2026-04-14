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
	m_logFilePath = (exePath.parent_path() / L"SvcHost.log").wstring();
	m_fileStream.open(m_logFilePath, std::ios::out | std::ios::trunc);

	if (!m_fileStream.is_open())
		m_fileStream.open(m_logFilePath, std::ios::out | std::ios::app);
}

void Logger::SetLogLevel(LogLevel level)
{
	std::lock_guard lock(m_mutex);
	m_level = level;
}

void Logger::SetLogFile(const std::wstring& path)
{
	std::lock_guard lock(m_mutex);

	if (m_fileStream.is_open())
		m_fileStream.close();

	m_logFilePath = path;
	m_fileStream.open(m_logFilePath, std::ios::out | std::ios::app);
}

void Logger::Log(LogLevel level, std::wstring_view message)
{
	if (level < m_level)
		return;

	std::wstring formatted = GetTimestamp() + L" [" + LevelToString(level) + L"] " + std::wstring(message);

	{
		std::lock_guard lock(m_mutex);
		if (m_fileStream.is_open())
		{
			m_fileStream << formatted << std::endl;
			m_fileStream.flush();
		}
	}

	// IDE
	OutputDebugStringW((formatted + L"\n").c_str());
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
