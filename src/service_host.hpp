#pragma once

#include <windows.h>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <filesystem>

struct HandleDeleter
{
	void operator()(HANDLE h) const noexcept;
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

struct LaunchResult
{
	UniqueHandle hProcess;
	UniqueHandle hThread;
	UniqueHandle hStdoutRead;
	UniqueHandle hStderrRead;
	DWORD		 dwProcessId;
};

class ServiceHost
{
public:
	ServiceHost()  = default;
	~ServiceHost() = default;

	ServiceHost(const ServiceHost&)			   = delete;
	ServiceHost& operator=(const ServiceHost&) = delete;
	ServiceHost(ServiceHost&&)				   = delete;
	ServiceHost& operator=(ServiceHost&&)	   = delete;

	[[nodiscard]] int Run(int argc, wchar_t* argv[]);

private:
	static void WINAPI						  _serviceMain(DWORD argc, LPWSTR* argv);
	static void WINAPI						  _serviceCtrlHandler(DWORD ctrl) noexcept;
	static void								  _updateStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) noexcept;
	static std::expected<LaunchResult, DWORD> _launchChild(std::wstring_view cmdLine);
	static UniqueHandle						  _createJobAndAssignProcess(HANDLE hProcess);
	static std::wstring						  _commandLine(DWORD argc, LPWSTR* argv);

	inline static std::wstring			_exe_name;
	inline static std::filesystem::path _exe_path;
	inline static std::filesystem::path _working_dir;

	inline static SERVICE_STATUS		_status{};
	inline static SERVICE_STATUS_HANDLE _status_handle{};
	inline static UniqueHandle			_h_stop_event;
	inline static UniqueHandle			_h_job;	   // Job object
};
