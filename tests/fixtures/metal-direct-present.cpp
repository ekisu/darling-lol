// Offline fixture of CAMetalDirect::Presenter (QuartzCore/CAMetalDirectPresent.h, extracted from patches/cocotron.patch) over a fake WSI.
// The fake follows the swapchain rules the core relies on: images are owned by the app (acquired), by us as spare, or by
// the presentation engine; the engine keeps the last presented image until a newer one arrives (flip-like) and
// releases older ones; acquire is non-blocking and fails with NotReady when no image is free; present fences signal
// some time after the present; host access to the swapchain must never be concurrent (checked).
#include "CAMetalDirectPresent.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <set>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); std::abort(); } } while (0)

enum class Owner { Free, App, Spare, Engine, Displayed };

struct FakeChain {
	explicit FakeChain(uint32_t images, uint32_t minImages): owners(images, Owner::Free), minImageCount(minImages) {}
	std::mutex mutex;              // the fake's internal state
	std::mutex hostMutex;           // what the real platform takes around acquire/present/retire (external sync)
	std::atomic<int> hostUsers {0}; // checks that hostMutex really serializes them
	std::vector<Owner> owners;
	uint32_t minImageCount;
	std::deque<uint32_t> engine;   // presented, not yet displayed
	int displayed = -1;
	uint64_t presentsIssued = 0, fencesSignalled = 0;
	bool retired = false;
	bool failNextPresent = false, failNextAcquire = false, failNextPrepare = false;
	std::vector<uint32_t> givenBack;
	std::atomic<int> idleBlock {0}; // >0: imagesIdle() reports busy

	void hostEnter() { hostMutex.lock(); CHECK(hostUsers.fetch_add(1) == 0); }
	void hostLeave() { hostUsers.fetch_sub(1); hostMutex.unlock(); }

	// The presentation engine: one queued image becomes displayed, the previous displayed one is freed.
	bool slowEngine = false;          // engine advances only from engineTick()
	void engineTick() { std::lock_guard<std::mutex> lock(mutex); bool slow = slowEngine; slowEngine = false; engineStep(); slowEngine = slow; }
	void engineStep() {
		if (slowEngine || engine.empty()) return;
		const uint32_t next = engine.front();
		engine.pop_front();
		if (displayed >= 0) owners[displayed] = Owner::Free;
		owners[next] = Owner::Displayed;
		displayed = (int) next;
		++fencesSignalled;
	}
};

struct FakePlatform {
	using Chain = FakeChain;
	std::atomic<uint64_t> acquires {0}, presents {0}, retires {0};

	CAMetalDirect::Status tryAcquire(Chain& chain, uint32_t& index) {
		chain.hostEnter();
		std::lock_guard<std::mutex> lock(chain.mutex);
		CHECK(!chain.retired);
		chain.engineStep(); // time passes
		CAMetalDirect::Status status = CAMetalDirect::Status::NotReady;
		if (chain.failNextAcquire) {
			chain.failNextAcquire = false;
			status = CAMetalDirect::Status::OutOfDate;
		} else {
			// forward-progress rule check: acquiring while holding more than imageCount - minImageCount may block forever
			uint32_t acquired = 0;
			for (auto owner: chain.owners) acquired += owner == Owner::App || owner == Owner::Spare;
			CHECK(acquired <= chain.owners.size() - chain.minImageCount);
			for (uint32_t i = 0; i < chain.owners.size(); ++i) {
				if (chain.owners[i] == Owner::Free) {
					chain.owners[i] = Owner::App;
					index = i;
					status = CAMetalDirect::Status::Ok;
					++acquires;
					break;
				}
			}
		}
		chain.hostLeave();
		return status;
	}

	bool prepareAcquired(Chain& chain, uint32_t index) {
		std::lock_guard<std::mutex> lock(chain.mutex);
		CHECK(chain.owners[index] == Owner::App);
		if (chain.failNextPrepare) {
			chain.failNextPrepare = false;
			chain.owners[index] = Owner::Spare;
			return false;
		}
		return true;
	}

	CAMetalDirect::Status present(Chain& chain, uint32_t index) {
		chain.hostEnter();
		std::lock_guard<std::mutex> lock(chain.mutex);
		CHECK(!chain.retired);
		// only images the app holds (fresh, or a spare the core handed out again) are presented, and only once
		CHECK(chain.owners[index] == Owner::App || chain.owners[index] == Owner::Spare);
		++chain.presentsIssued;
		++presents;
		CAMetalDirect::Status status = CAMetalDirect::Status::Ok;
		if (chain.failNextPresent) {
			chain.failNextPresent = false;
			status = CAMetalDirect::Status::OutOfDate;
		}
		chain.owners[index] = Owner::Engine;
		chain.engine.push_back(index);
		chain.hostLeave();
		return status;
	}

	bool presentsComplete(Chain& chain) {
		std::lock_guard<std::mutex> lock(chain.mutex);
		chain.engineStep();
		return chain.engine.empty();
	}

	bool imagesIdle(Chain& chain, const std::vector<uint32_t>& images) {
		std::lock_guard<std::mutex> lock(chain.mutex);
		for (uint32_t i: images) CHECK(chain.owners[i] == Owner::Spare || chain.owners[i] == Owner::App);
		return chain.idleBlock.load() == 0;
	}

	void retire(Chain& chain, const std::vector<uint32_t>& spare) {
		chain.hostEnter();
		std::lock_guard<std::mutex> lock(chain.mutex);
		CHECK(!chain.retired);
		CHECK(chain.engine.empty());
		// every image is the displayed one, free, or given back now: nothing is held by the application
		std::set<uint32_t> back(spare.begin(), spare.end());
		CHECK(back.size() == spare.size());
		for (uint32_t i = 0; i < chain.owners.size(); ++i) {
			if (back.count(i)) {
				CHECK(chain.owners[i] == Owner::Spare || chain.owners[i] == Owner::App);
			} else {
				CHECK(chain.owners[i] == Owner::Free || chain.owners[i] == Owner::Displayed);
			}
		}
		chain.givenBack = spare;
		chain.retired = true;
		++retires;
		chain.hostLeave();
	}

	void waitForProgress(std::unique_lock<std::mutex>& lock, std::condition_variable& condition, std::chrono::steady_clock::time_point until) {
		condition.wait_until(lock, until);
	}
};

using Presenter = CAMetalDirect::Presenter<FakePlatform>;
using Clock = std::chrono::steady_clock;

static std::shared_ptr<FakeChain> chainFor(uint32_t minImages, uint32_t maxHeld) {
	return std::make_shared<FakeChain>(minImages + maxHeld - 1, minImages);
}

static Clock::time_point in(int ms) { return Clock::now() + std::chrono::milliseconds(ms); }

// The application marks images it abandons as spare in the fake (the core keeps them acquired).
static void abandon(Presenter& presenter, const Presenter::Image& image) {
	{
		std::lock_guard<std::mutex> lock(image.chain->mutex);
		CHECK(image.chain->owners[image.index] == Owner::App || image.chain->owners[image.index] == Owner::Spare);
		image.chain->owners[image.index] = Owner::Spare;
	}
	presenter.abandon(image);
}

static void retireBlocking(Presenter& presenter) {
	const auto deadline = in(2000);
	while (!presenter.tryRetire()) {
		CHECK(Clock::now() < deadline);
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

static void testBasic() {
	FakePlatform platform;
	Presenter presenter(platform);
	auto chain = chainFor(3, 3);
	CHECK(presenter.beginActivation(chain, 3));
	CHECK(presenter.completeActivation());
	for (int frame = 0; frame < 2000; ++frame) {
		Presenter::Image image;
		CHECK(presenter.acquire(image, in(1000)) == CAMetalDirect::AcquireResult::Acquired);
		CHECK(presenter.present(image) == CAMetalDirect::Status::Ok);
	}
	CHECK(presenter.held() == 0);
	CHECK(presenter.revoke() == CAMetalDirect::State::Draining);
	retireBlocking(presenter);
	CHECK(presenter.state() == CAMetalDirect::State::Inactive && chain->retired);
	CHECK(presenter.counters().presented == 2000);
	printf("basic ok\n");
}

// Holding maxHeld images: the next acquisition times out (bounded) and never asks the WSI for an image it may never get.
static void testHeldBound() {
	FakePlatform platform;
	Presenter presenter(platform);
	auto chain = chainFor(3, 3);
	presenter.beginActivation(chain, 3);
	presenter.completeActivation();
	Presenter::Image held[3];
	for (auto& image: held) CHECK(presenter.acquire(image, in(1000)) == CAMetalDirect::AcquireResult::Acquired);
	Presenter::Image extra;
	const auto start = Clock::now();
	CHECK(presenter.acquire(extra, in(50)) == CAMetalDirect::AcquireResult::TimedOut);
	CHECK(Clock::now() - start >= std::chrono::milliseconds(45));
	// a present from another thread wakes the waiting acquisition
	std::thread worker([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); presenter.present(held[0]); });
	const auto woken = Clock::now();
	CHECK(presenter.acquire(extra, in(1000)) == CAMetalDirect::AcquireResult::Acquired);
	CHECK(Clock::now() - woken < std::chrono::milliseconds(500)); // woken by the present, not by the deadline
	worker.join();
	presenter.present(held[1]);
	presenter.present(held[2]);
	presenter.present(extra);
	presenter.revoke();
	retireBlocking(presenter);
	printf("held bound ok\n");
}

static void testSpareReuse() {
	FakePlatform platform;
	Presenter presenter(platform);
	auto chain = chainFor(3, 2);
	presenter.beginActivation(chain, 2);
	presenter.completeActivation();
	Presenter::Image a, b;
	CHECK(presenter.acquire(a, in(100)) == CAMetalDirect::AcquireResult::Acquired);
	const uint64_t acquiresBefore = platform.acquires;
	abandon(presenter, a);
	CHECK(presenter.acquire(b, in(100)) == CAMetalDirect::AcquireResult::Acquired);
	CHECK(b.index == a.index && platform.acquires == acquiresBefore); // handed out again, no WSI acquire
	{
		std::lock_guard<std::mutex> lock(chain->mutex);
		chain->owners[b.index] = Owner::App;
	}
	abandon(presenter, b);
	// retirement gives the spare back, only once the GPU is done with it
	chain->idleBlock = 1;
	presenter.revoke();
	for (int i = 0; i < 20; ++i) CHECK(!presenter.tryRetire());
	chain->idleBlock = 0;
	retireBlocking(presenter);
	CHECK(chain->givenBack.size() == 1 && chain->givenBack[0] == a.index);
	printf("spare reuse ok\n");
}

static void testActivating() {
	FakePlatform platform;
	Presenter presenter(platform);
	auto chain = chainFor(3, 3);
	presenter.beginActivation(chain, 3);
	Presenter::Image image;
	CHECK(presenter.acquire(image, in(30)) == CAMetalDirect::AcquireResult::TimedOut);
	std::thread mainThread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); presenter.completeActivation(); });
	CHECK(presenter.acquire(image, in(1000)) == CAMetalDirect::AcquireResult::Acquired);
	mainThread.join();
	presenter.present(image);
	presenter.revoke();
	retireBlocking(presenter);
	// revoked while activating: nothing was acquired, the chain is retired at once
	auto chain2 = chainFor(3, 3);
	presenter.beginActivation(chain2, 3);
	CHECK(presenter.revoke() == CAMetalDirect::State::Inactive && chain2->retired);
	CHECK(presenter.acquire(image, in(10)) == CAMetalDirect::AcquireResult::NotActive);
	printf("activating ok\n");
}

static void testFailures() {
	FakePlatform platform;
	Presenter presenter(platform);
	auto chain = chainFor(3, 3);
	presenter.beginActivation(chain, 3);
	presenter.completeActivation();
	Presenter::Image image;
	chain->failNextAcquire = true;
	CHECK(presenter.acquire(image, in(100)) == CAMetalDirect::AcquireResult::Failed);
	CHECK(presenter.state() == CAMetalDirect::State::Draining);
	retireBlocking(presenter);

	auto chain2 = chainFor(3, 3);
	presenter.beginActivation(chain2, 3);
	presenter.completeActivation();
	CHECK(presenter.acquire(image, in(100)) == CAMetalDirect::AcquireResult::Acquired);
	Presenter::Image second;
	CHECK(presenter.acquire(second, in(100)) == CAMetalDirect::AcquireResult::Acquired);
	chain2->failNextPresent = true;
	CHECK(presenter.present(image) == CAMetalDirect::Status::OutOfDate);
	CHECK(presenter.state() == CAMetalDirect::State::Draining);
	CHECK(!presenter.tryRetire()); // `second` is still held
	Presenter::Image third;
	CHECK(presenter.acquire(third, in(10)) == CAMetalDirect::AcquireResult::NotActive);
	CHECK(presenter.present(second) == CAMetalDirect::Status::Ok); // images of a draining chain are still presented
	retireBlocking(presenter);

	auto chain3 = chainFor(3, 3);
	presenter.beginActivation(chain3, 3);
	presenter.completeActivation();
	chain3->failNextPrepare = true;
	CHECK(presenter.acquire(image, in(100)) == CAMetalDirect::AcquireResult::Failed);
	retireBlocking(presenter);
	CHECK(chain3->givenBack.size() == 1);
	printf("failures ok\n");
}

// Application threads acquire and hand images to a single worker that presents them (as Indium's submit worker);
// the main thread revokes, retires and reactivates at random; some acquisitions are abandoned.
static void testStress(int seconds) {
	FakePlatform platform;
	Presenter presenter(platform);
	std::mutex queueMutex;
	std::condition_variable queueCondition;
	std::deque<Presenter::Image> queue;
	std::atomic<bool> stop {false};
	std::vector<std::shared_ptr<FakeChain>> chains;
	std::atomic<uint64_t> acquired {0}, abandoned {0}, notActive {0}, timedOut {0};

	auto activate = [&] {
		auto chain = chainFor(3, 3);
		chains.push_back(chain);
		CHECK(presenter.beginActivation(chain, 3));
		presenter.completeActivation();
	};
	activate();

	std::thread worker([&] {
		for (;;) {
			Presenter::Image image;
			{
				std::unique_lock<std::mutex> lock(queueMutex);
				queueCondition.wait(lock, [&] { return !queue.empty() || stop; });
				if (queue.empty()) return;
				image = queue.front();
				queue.pop_front();
			}
			presenter.present(image);
		}
	});
	std::vector<std::thread> apps;
	for (int t = 0; t < 2; ++t) {
		apps.emplace_back([&, t] {
			std::mt19937 random(1234 + t);
			while (!stop) {
				Presenter::Image image;
				const auto result = presenter.acquire(image, in(5));
				if (result == CAMetalDirect::AcquireResult::Acquired) {
					++acquired;
					if (random() % 17 == 0) {
						abandon(presenter, image);
						++abandoned;
					} else {
						std::lock_guard<std::mutex> lock(queueMutex);
						queue.push_back(image);
						queueCondition.notify_one();
					}
				} else if (result == CAMetalDirect::AcquireResult::NotActive) {
					++notActive;
					std::this_thread::yield();
				} else {
					++timedOut;
				}
			}
		});
	}
	std::mt19937 random(99);
	const auto end = Clock::now() + std::chrono::seconds(seconds);
	uint64_t cycles = 0;
	while (Clock::now() < end) {
		std::this_thread::sleep_for(std::chrono::microseconds(random() % 3000));
		if (random() % 7 == 0) {
			std::lock_guard<std::mutex> lock(chains.back()->mutex);
			chains.back()->failNextPresent = true;
		}
		presenter.revoke();
		const auto deadline = in(2000);
		while (!presenter.tryRetire()) {
			CHECK(Clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		CHECK(chains.back()->retired);
		activate();
		++cycles;
	}
	stop = true;
	for (auto& app: apps) app.join();
	queueCondition.notify_all();
	worker.join();
	presenter.revoke();
	retireBlocking(presenter);
	for (auto& chain: chains) CHECK(chain->retired);
	CHECK(presenter.held() == 0 && !presenter.lostSpare());
	const auto c = presenter.counters();
	printf("stress ok: cycles=%llu acquired=%llu abandoned=%llu notActive=%llu timedOut=%llu presented=%llu presentFailures=%llu polls=%llu\n",
		(unsigned long long) cycles, (unsigned long long) acquired.load(), (unsigned long long) abandoned.load(),
		(unsigned long long) notActive.load(), (unsigned long long) timedOut.load(), (unsigned long long) c.presented,
		(unsigned long long) c.presentFailures, (unsigned long long) c.acquirePolls);
	CHECK(cycles > 10 && c.presented > 100);
}

// The presentation engine frees images only at its own pace: acquisitions find no free image, poll, and are woken.
static void testSlowEngine() {
	FakePlatform platform;
	Presenter presenter(platform);
	auto chain = chainFor(3, 3);
	chain->slowEngine = true;
	presenter.beginActivation(chain, 3);
	presenter.completeActivation();
	std::atomic<bool> stop {false};
	std::thread engine([&] { while (!stop) { chain->engineTick(); std::this_thread::sleep_for(std::chrono::microseconds(700)); } });
	uint64_t frames = 0, timeouts = 0;
	const auto end = in(500);
	while (Clock::now() < end) {
		Presenter::Image image;
		const auto result = presenter.acquire(image, in(50));
		if (result == CAMetalDirect::AcquireResult::TimedOut) { ++timeouts; continue; }
		CHECK(result == CAMetalDirect::AcquireResult::Acquired);
		presenter.present(image);
		++frames;
	}
	presenter.revoke();
	retireBlocking(presenter);
	stop = true;
	engine.join();
	const auto c = presenter.counters();
	printf("slow engine ok: frames=%llu timeouts=%llu polls=%llu\n", (unsigned long long) frames, (unsigned long long) timeouts, (unsigned long long) c.acquirePolls);
	CHECK(frames > 100 && c.acquirePolls > 0 && timeouts == 0);
}

// An acquisition polling for a free image returns as soon as the layer is revoked (not at its deadline).
static void testRevokeWhilePolling() {
	FakePlatform platform;
	Presenter presenter(platform);
	auto chain = chainFor(3, 3); // 5 images
	chain->slowEngine = true;    // the engine never frees anything in this test
	presenter.beginActivation(chain, 3);
	presenter.completeActivation();
	Presenter::Image images[5];
	for (int i = 0; i < 3; ++i) CHECK(presenter.acquire(images[i], in(100)) == CAMetalDirect::AcquireResult::Acquired);
	for (int i = 0; i < 3; ++i) presenter.present(images[i]);
	for (int i = 3; i < 5; ++i) CHECK(presenter.acquire(images[i], in(100)) == CAMetalDirect::AcquireResult::Acquired);
	std::thread mainThread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); presenter.revoke(); });
	Presenter::Image polling;
	const auto start = Clock::now();
	CHECK(presenter.acquire(polling, in(2000)) == CAMetalDirect::AcquireResult::NotActive);
	CHECK(Clock::now() - start < std::chrono::milliseconds(500));
	mainThread.join();
	presenter.present(images[3]);
	presenter.present(images[4]);
	{
		std::lock_guard<std::mutex> lock(chain->mutex);
		chain->slowEngine = false;
	}
	retireBlocking(presenter);
	printf("revoke while polling ok\n");
}

int main(int argc, char** argv) {
	testRevokeWhilePolling();
	testSlowEngine();
	testBasic();
	testHeldBound();
	testSpareReuse();
	testActivating();
	testFailures();
	testStress(argc > 1 ? atoi(argv[1]) : 3);
	printf("all ok\n");
	return 0;
}
