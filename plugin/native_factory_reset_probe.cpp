// Verifies D110CoreNative::factoryReset() the same way the MAME-backed tool's output was
// checked by hand earlier this session: patch name "Patch   01", reserve settings summing to
// 32, channel assignment reading 1..9 - all at their known RAM offsets (D110Core.cpp's own
// kMirrorRegions comments). Starts from a virgin (never-reset) nvram dir, same as a factory-
// fresh unit, and does the reset entirely through the native core - no MAME involved anywhere.
#include "Source/native/D110CoreNative.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// Usage: native_factory_reset_probe <romFolder> [nvramDir] - the ROM folder is whichever one the app is
	// configured to use (no default: it is a user choice, not tied to any plugin format). A fresh, empty
	// nvramDir is the point (a virgin unit).
	if (argc < 2) {
		std::printf("usage: %s <romFolder> [nvramDir]\n", argv[0]);
		return 2;
	}
	const std::string romDir = argv[1];
	const char *nvramDir = argc > 2 ? argv[2] : "/tmp/native_factory_reset_test";

	D110CoreNative core;
	if (!core.start(romDir, nvramDir)) {
		std::printf("failed to start\n");
		return 1;
	}
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);

	std::printf("booting (3s)...\n");
	core.runForSeconds(3.0);

	std::printf("factory reset...\n");
	core.factoryReset();
	std::printf("done (isResetting=%s)\n", core.isResetting() ? "true" : "false");

	uint8_t ram[D110CoreNative::kRamSize];
	core.getRam(ram);

	bool ok = true;
	auto check = [&](const char *label, bool cond) {
		std::printf("%-40s %s\n", label, cond ? "PASS" : "FAIL");
		ok &= cond;
	};

	check("patch name == \"Patch   01\"", std::memcmp(ram, "Patch   01", 10) == 0);

	int reserveSum = 0;
	for (int i = 0; i < 9; ++i) reserveSum += ram[0x2D98 + i];
	check("reserve settings sum to 32", reserveSum == 32);

	bool chanOk = true;
	for (int i = 0; i < 9; ++i) if (ram[0x2D98 + 9 + i] != i + 1) chanOk = false;
	check("channel assignment reads 1..9", chanOk);

	core.stop();
	std::printf("\n%s\n", ok ? "ALL PASS" : "SOME FAILED");
	return ok ? 0 : 1;
}
