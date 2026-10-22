// Host checks of Indium's startup configuration helper (private-include/indium/startup-config.private.hpp, extracted from
// patches/metal--deps--indium.patch): environment over guest file over default, "0"/empty handling, number parsing and
// clamping, and that a call site resolves its value with exactly one lookup however often it runs.
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// Count the filesystem lookups the helper makes (the header is included after the system headers, so only its calls
// are renamed).
static int accessCalls = 0, fopenCalls = 0;
static int countedAccess(const char* path, int mode) { ++accessCalls; return access(path, mode); }
static FILE* countedFopen(const char* path, const char* mode) { ++fopenCalls; return fopen(path, mode); }
#define access countedAccess
#define fopen countedFopen
#include <indium/startup-config.private.hpp>
#undef access
#undef fopen

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); return 1; } } while (0)

using namespace Indium::StartupConfig;

static std::string dir;
static std::string path(const char* name) { return dir + "/" + name; }
static void write(const char* name, const char* text) { FILE* f = ::fopen(path(name).c_str(), "w"); fputs(text, f); fclose(f); }

// A call site the way the runtime writes them: resolved once, then a cached value.
static bool siteFlag(const char* file) { static const bool value = flag("DARLING_METAL_TEST_SITE", file); return value; }
static long siteNumber(const char* file) { static const long value = number("DARLING_METAL_TEST_NUMBER", file, 500, 500, 100000); return value; }

int main() {
#ifdef __linux__
	// The mutation controls make this fixture fail on purpose (abort): no core dump, no crash report.
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
	char templ[] = "/tmp/metal-startup-config-XXXXXX";
	CHECK(mkdtemp(templ));
	dir = templ;
	const std::string flagFile = path("flag"), missing = path("missing");

	// flags: default off, file on, environment wins either way
	unsetenv("DARLING_METAL_TEST");
	CHECK(!flag("DARLING_METAL_TEST", missing.c_str()));
	CHECK(!flag("DARLING_METAL_TEST", nullptr));
	write("flag", "");
	CHECK(flag("DARLING_METAL_TEST", flagFile.c_str()));
	setenv("DARLING_METAL_TEST", "0", 1);
	CHECK(!flag("DARLING_METAL_TEST", flagFile.c_str()));
	setenv("DARLING_METAL_TEST", "1", 1);
	CHECK(flag("DARLING_METAL_TEST", missing.c_str()));
	setenv("DARLING_METAL_TEST", "yes", 1);
	CHECK(flag("DARLING_METAL_TEST", missing.c_str()));
	setenv("DARLING_METAL_TEST", "00", 1);
	CHECK(flag("DARLING_METAL_TEST", missing.c_str())); // only exactly "0" is off
	setenv("DARLING_METAL_TEST", "", 1); // empty = unset: the file decides
	CHECK(flag("DARLING_METAL_TEST", flagFile.c_str()));
	CHECK(!flag("DARLING_METAL_TEST", missing.c_str()));
	// the environment answers without touching the filesystem
	setenv("DARLING_METAL_TEST", "1", 1);
	accessCalls = 0;
	CHECK(flag("DARLING_METAL_TEST", flagFile.c_str()) && accessCalls == 0);

	// activation with a build default and the former on/off guest files
	unsetenv("DARLING_METAL_ACT");
	const std::string on = path("on"), off = path("off");
	CHECK(!activation("DARLING_METAL_ACT", false, on.c_str(), off.c_str()));
	CHECK(activation("DARLING_METAL_ACT", true, on.c_str(), off.c_str()));
	write("on", "");
	write("off", "");
	CHECK(activation("DARLING_METAL_ACT", false, on.c_str(), off.c_str())); // default off: the on file enables
	CHECK(!activation("DARLING_METAL_ACT", true, on.c_str(), off.c_str())); // default on: the off file disables
	setenv("DARLING_METAL_ACT", "1", 1);
	CHECK(activation("DARLING_METAL_ACT", true, on.c_str(), off.c_str()));
	setenv("DARLING_METAL_ACT", "0", 1);
	CHECK(!activation("DARLING_METAL_ACT", false, on.c_str(), off.c_str()));
	setenv("DARLING_METAL_ACT", "", 1);
	CHECK(activation("DARLING_METAL_ACT", false, on.c_str(), off.c_str()));
	unlink(on.c_str());
	unlink(off.c_str());

	// numbers: default, file, environment, clamping, rejection without clamp, invalid text
	unsetenv("DARLING_METAL_N");
	CHECK(number("DARLING_METAL_N", missing.c_str(), 16, 1, 256) == 16);
	write("n", "64\n");
	CHECK(number("DARLING_METAL_N", path("n").c_str(), 16, 1, 256) == 64);
	write("n", "  999\n");
	CHECK(number("DARLING_METAL_N", path("n").c_str(), 16, 1, 256) == 256);
	write("n", "abc");
	CHECK(number("DARLING_METAL_N", path("n").c_str(), 16, 1, 256) == 16);
	write("n", "12x");
	CHECK(number("DARLING_METAL_N", path("n").c_str(), 16, 1, 256) == 16);
	setenv("DARLING_METAL_N", "5000", 1);
	CHECK(number("DARLING_METAL_N", path("n").c_str(), 500, 500, 100000) == 5000);
	setenv("DARLING_METAL_N", "100", 1);
	CHECK(number("DARLING_METAL_N", missing.c_str(), 500, 500, 100000) == 500); // clamped up
	setenv("DARLING_METAL_N", "200000", 1);
	CHECK(number("DARLING_METAL_N", missing.c_str(), 500, 500, 100000) == 100000); // clamped down
	setenv("DARLING_METAL_N", "-3", 1);
	CHECK(number("DARLING_METAL_N", missing.c_str(), 16, 0, 16, false) == 16); // outside without clamp: default
	setenv("DARLING_METAL_N", "17", 1);
	CHECK(number("DARLING_METAL_N", missing.c_str(), 0, 0, 16, false) == 0);
	setenv("DARLING_METAL_N", "4", 1);
	CHECK(number("DARLING_METAL_N", missing.c_str(), 0, 0, 16, false) == 4);
	setenv("DARLING_METAL_N", "99999999999999999999999", 1);
	CHECK(number("DARLING_METAL_N", missing.c_str(), 16, 1, 256) == 16); // overflow: default
	setenv("DARLING_METAL_N", "", 1);
	CHECK(number("DARLING_METAL_N", path("n").c_str(), 16, 1, 256) == 16); // empty env: file ("12x") is invalid

	// call sites: one lookup, and later changes of the file or the environment are not seen in this process
	unsetenv("DARLING_METAL_TEST_SITE");
	unsetenv("DARLING_METAL_TEST_NUMBER");
	write("site", "");
	write("number", "2000");
	accessCalls = fopenCalls = 0;
	for (int i = 0; i < 100000; ++i) {
		CHECK(siteFlag(path("site").c_str()));
		CHECK(siteNumber(path("number").c_str()) == 2000);
		if (i == 10) {
			unlink(path("site").c_str());
			write("number", "9000");
			setenv("DARLING_METAL_TEST_SITE", "0", 1);
			setenv("DARLING_METAL_TEST_NUMBER", "700", 1);
		}
	}
	CHECK(accessCalls == 1 && fopenCalls == 1);

	unlink(flagFile.c_str());
	unlink(path("n").c_str());
	unlink(path("number").c_str());
	rmdir(dir.c_str());
	printf("all ok\n");
	return 0;
}
