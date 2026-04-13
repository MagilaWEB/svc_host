#pragma once

#include <windows.h>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <span>

struct HandleDeleter
{
	void operator()(HANDLE h) const noexcept;
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

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
	static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
	static void WINAPI ServiceCtrlHandler(DWORD ctrl) noexcept;

	static void										 UpdateStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) noexcept;

	static std::expected<PROCESS_INFORMATION, DWORD> LaunchChild(std::wstring_view cmdLine);
	static UniqueHandle								 CreateJobAndAssignProcess(HANDLE hProcess);

	inline static SERVICE_STATUS		s_Status{};
	inline static SERVICE_STATUS_HANDLE s_StatusHandle{};
	inline static UniqueHandle			s_hChildProcess;
	inline static UniqueHandle			s_hStopEvent;
	inline static UniqueHandle			s_hJob;	   // Job object
};
