// Host checks of Indium's submit worker pacing (private-include/indium/submit-thread.private.hpp, extracted from
// patches/metal--deps--indium.patch): post() sequence numbers, waitFor() not extended by later tasks, postPaced()
// keeping a committing thread at most one presenting task ahead of the worker, and drainFor() bounded at exit.
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <indium/submit-thread.private.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace std::chrono_literals;
namespace ST = Indium::SubmitThread;

static uint64_t completed() {
	auto& s = ST::state();
	std::lock_guard<std::mutex> lock(s.mutex);
	return s.completed;
}

// A task that blocks the worker until released.
struct Gate {
	std::promise<void> open;
	std::shared_future<void> opened = open.get_future().share();
	std::atomic<bool> entered {false};
	std::function<void()> task() {
		auto f = opened;
		auto* e = &entered;
		return [f, e] { e->store(true); f.wait(); };
	}
	void release() { open.set_value(); }
};

static void settle() {
	// wait until every posted task finished (the tests leave the worker idle between them)
	CHECK(ST::drainFor(5000ms));
}

static void testSequenceAndOrder() {
	std::vector<int> order;
	std::mutex lock;
	const uint64_t a = ST::post([&] { std::lock_guard<std::mutex> l(lock); order.push_back(1); });
	const uint64_t b = ST::post([&] { std::lock_guard<std::mutex> l(lock); order.push_back(2); });
	CHECK(b == a + 1);
	ST::waitFor(b, "test");
	CHECK(completed() >= b);
	CHECK((order == std::vector<int> {1, 2}));
	ST::waitFor(0, "test"); // sequence 0: nothing to wait for
}

static void testWaitForIgnoresLaterTasks() {
	const uint64_t mine = ST::post([] {});
	Gate later;
	ST::post(later.task());
	auto waited = std::async(std::launch::async, [&] { ST::waitFor(mine, "test"); });
	CHECK(waited.wait_for(2s) == std::future_status::ready); // must not wait for the blocked later task
	later.release();
	settle();
}

static void testPacedStaysOneFrameAhead() {
	// frame 1's task is blocked on the worker: its own paced commit returns (only the previous frame is awaited)
	Gate frame1;
	auto first = std::async(std::launch::async, [&] { return ST::postPaced(frame1.task(), "test"); });
	CHECK(first.wait_for(2s) == std::future_status::ready);
	const uint64_t s1 = first.get();
	// frame 2 must wait for frame 1 to finish
	std::atomic<bool> frame2Ran {false};
	auto second = std::async(std::launch::async, [&] { return ST::postPaced([&] { frame2Ran = true; }, "test"); });
	CHECK(second.wait_for(300ms) == std::future_status::timeout);
	CHECK(completed() < s1);
	frame1.release();
	CHECK(second.wait_for(2s) == std::future_status::ready);
	second.get();
	CHECK(completed() >= s1);
	settle();
	CHECK(frame2Ran);
}

static void testPacedBoundUnderLoad() {
	// many frames from two threads with a slow worker: after each paced commit returns, at most one presenting task
	// (the caller's own) may still be unfinished among the presenting tasks
	std::atomic<uint64_t> finishedPresenting {0}, issued {0};
	std::atomic<int> violations {0};
	auto producer = [&] {
		for (int i = 0; i < 200; ++i) {
			ST::post([] { std::this_thread::sleep_for(50us); }); // non-presenting work of the frame
			ST::postPaced([&] { std::this_thread::sleep_for(100us); ++finishedPresenting; }, "test");
			const uint64_t sent = ++issued;
			// every presenting task issued before this one (by either thread) except at most one must be done
			if (finishedPresenting.load() + 2 < sent) ++violations;
		}
	};
	std::thread a(producer), b(producer);
	a.join();
	b.join();
	settle();
	CHECK(finishedPresenting == 400);
	CHECK(violations == 0);
}

static void testDrainForIsBounded() {
	Gate stuck;
	ST::post(stuck.task());
	const auto start = std::chrono::steady_clock::now();
	CHECK(!ST::drainFor(200ms));
	CHECK(std::chrono::steady_clock::now() - start < 2s);
	stuck.release();
	CHECK(ST::drainFor(5000ms));
}

int main() {
#ifdef __linux__
	// The mutation controls make this fixture fail on purpose (abort): no core dump, no crash report.
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
	ST::ensureStarted();
	testSequenceAndOrder();
	testWaitForIgnoresLaterTasks();
	testPacedStaysOneFrameAhead();
	testPacedBoundUnderLoad();
	testDrainForIsBounded();
	printf("all ok\n");
	return 0;
}
