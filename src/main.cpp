#include "service_host.hpp"

int main()
{
	int		argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
		return 1;

	int result = ServiceHost{}.Run(argc, argv);

	LocalFree(argv);
	return result;
}
