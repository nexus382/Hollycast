// Developer-only entry point. No runtime JSON input is needed by the emulator.
#include "arm64_aot_offline.h"
#include <cstdio>

int main(int argc, char **argv)
{
	if (argc != 3)
	{
		fprintf(stderr, "Usage: aot-offline-emit fixture.json output-directory\n");
		return 2;
	}
	const auto path = arm64AotEmitBenchFixture(argv[1], argv[2]);
	if (path.empty())
	{
		fprintf(stderr, "Cannot emit fixture: invalid input or output path\n");
		return 1;
	}
	fprintf(stdout, "%s\n", path.c_str());
	return 0;
}
