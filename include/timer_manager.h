#pragma once

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

// 单线程定时器：epoll + timerfd + 最小堆。回调在定时器线程中执行，执行期间不持有内部锁，回调里可以再加/取消定时器。
// cancel() 只保证尚未开始执行的回调不再执行；已经开始执行的回调它不会等待（调用者常常持有回调也要获取的锁，等待会死锁），
// 所以回调必须自己核对要处理的对象/状态是否仍然有效。
class TimerManager {
public:
	using TimerCallback = std::function<void(void*)>;
	// 64 位且单调递增：每次 waitpostdata/getresponse/延时投递都会分配一个 ID，32 位在高负载下几天就会溢出回绕
	using TimerId = int64_t;

	TimerManager() :
		epoll_fd_(epoll_create1(EPOLL_CLOEXEC)),
		timer_fd_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
		wakeup_fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
		running_(false) {

		if (epoll_fd_ == -1 || timer_fd_ == -1 || wakeup_fd_ == -1) {
			throw std::runtime_error("Failed to create timer resources");
		}

		// 将timer_fd_加入epoll监控
		epoll_event ev;
		ev.events = EPOLLIN | EPOLLET;
		ev.data.fd = timer_fd_;
		epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);

		// 将wakeup_fd_加入epoll监控
		epoll_event ev_wake{};
		ev_wake.events = EPOLLIN;
		ev_wake.data.fd = wakeup_fd_;
		epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev_wake);
	}

	~TimerManager() {
		stop();
		close(timer_fd_);
		close(wakeup_fd_);
		close(epoll_fd_);
	}

	void start() {
		if (!running_.exchange(true)) {
			worker_thread_ = std::thread(&TimerManager::processTimers, this);
		}
	}

	// 停止定时器线程并等待它退出；返回后不会再有回调执行。不能在回调里调用
	void stop() {
		if (running_.exchange(false)) {
			uint64_t one = 1;
			write(wakeup_fd_, &one, sizeof(one)); // 正确唤醒方式
			if (worker_thread_.joinable()) {
				worker_thread_.join();
			}
		}
	}

	TimerId add_periodic(uint64_t interval_ms, TimerCallback cb, void* arg) {
		return addTimer(interval_ms, interval_ms, std::move(cb), arg);
	}

	TimerId add_once(uint64_t delay_ms, TimerCallback cb, void* arg) {
		return addTimer(delay_ms, 0, std::move(cb), arg);
	}

	// 返回 true 表示该定时器存在且已标记取消（回调若已开始执行，仍会执行完，见类注释）
	bool cancel(TimerId timer_id) {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = timers_.find(timer_id);
		if (it == timers_.end()) return false;

		std::shared_ptr<TimerNode> timer = it->second;
		timer->valid = false; // 惰性删除：仍在堆中的节点到期弹出时跳过
		timers_.erase(it);

		if (timer->in_heap) {
			++cancelled_in_heap_;
			if (heap_.front() == timer) {
				// 取消的是下一个要触发的定时器：去掉堆顶的已取消节点，按新的堆顶重置timerfd
				while (!heap_.empty() && !heap_.front()->valid) {
					popHeap();
					--cancelled_in_heap_;
				}
				resetTimerFd();
			}
			else if (cancelled_in_heap_ >= kCompactThreshold && cancelled_in_heap_ * 2 > heap_.size()) {
				// 已取消的节点要到原定到期时间才弹出，超时长、取消频繁（waitpostdata/getresponse）时会大量堆积；
				// 超过一半时整体清理一次，均摊 O(1)
				compactHeap();
			}
		}
		return true;
	}

private:
	struct TimerNode {
		uint64_t expire;      // 绝对到期时间(ms)
		uint64_t interval;    // 间隔时间(ms)
		TimerId id;           // 定时器ID
		TimerCallback cb;     // 回调函数
		void* arg;            // 回调参数
		bool valid = true;    // 是否有效
		bool periodic;        // 是否周期性
		bool in_heap = false; // 是否在堆中（回调执行期间不在堆中）
	};

	struct TimerCompare {
		bool operator()(const std::shared_ptr<TimerNode>& a,
			const std::shared_ptr<TimerNode>& b) const {
			// 最小堆；同一毫秒到期的按创建顺序触发（同一 tag 连续两次写的延时投递不会颠倒）
			if (a->expire != b->expire)
				return a->expire > b->expire;
			return a->id > b->id;
		}
	};

	static constexpr size_t kCompactThreshold = 1024;

	int epoll_fd_;
	int timer_fd_;
	int wakeup_fd_; // 新增专用唤醒fd
	std::atomic<bool> running_;
	std::thread worker_thread_;
	std::mutex mutex_;
	std::unordered_map<TimerId, std::shared_ptr<TimerNode>> timers_;	// 未取消、未结束的定时器
	std::vector<std::shared_ptr<TimerNode>> heap_;					// 按 TimerCompare 组织的最小堆，含尚未弹出的已取消节点
	size_t cancelled_in_heap_ = 0;									// heap_ 中已取消节点的个数
	TimerId next_id_ = 1;

	void pushHeap(std::shared_ptr<TimerNode> timer) {
		timer->in_heap = true;
		heap_.push_back(std::move(timer));
		std::push_heap(heap_.begin(), heap_.end(), TimerCompare());
	}

	std::shared_ptr<TimerNode> popHeap() {
		std::pop_heap(heap_.begin(), heap_.end(), TimerCompare());
		std::shared_ptr<TimerNode> timer = std::move(heap_.back());
		heap_.pop_back();
		timer->in_heap = false;
		return timer;
	}

	void compactHeap() {
		heap_.erase(std::remove_if(heap_.begin(), heap_.end(), [](const std::shared_ptr<TimerNode>& t) {
			if (t->valid) return false;
			t->in_heap = false;
			return true;
			}), heap_.end());
		std::make_heap(heap_.begin(), heap_.end(), TimerCompare());
		cancelled_in_heap_ = 0;
	}

	TimerId addTimer(uint64_t delay_ms, uint64_t interval_ms, TimerCallback cb, void* arg) {
		std::lock_guard<std::mutex> lock(mutex_);

		auto timer = std::make_shared<TimerNode>();
		timer->id = next_id_++;
		timer->expire = getCurrentMs() + delay_ms;
		timer->interval = interval_ms;
		timer->cb = std::move(cb);
		timer->arg = arg;
		timer->periodic = (interval_ms > 0);

		bool is_next_timer = heap_.empty() || (timer->expire < heap_.front()->expire);
		timers_[timer->id] = timer;
		pushHeap(timer);

		if (is_next_timer) {
			resetTimerFd();

			// 下面的代码有误，不能通过wakeup_fd_唤醒epoll_wait
			// 强制唤醒epoll_wait以立即处理新定时器
			//uint64_t one = 1;
			//write(timer_fd_, &one, sizeof(one));

			// 正确唤醒方式
			uint64_t one = 1;
			write(wakeup_fd_, &one, sizeof(one)); 
		}

		return timer->id;
	}

	void resetTimerFd() {
		if (heap_.empty()) {
			// 没有定时器，禁用timerfd
			struct itimerspec its = {};
			timerfd_settime(timer_fd_, 0, &its, nullptr);
			return;
		}

		const auto& next_timer = heap_.front();
		uint64_t now = getCurrentMs();
		uint64_t delay = (next_timer->expire > now) ? (next_timer->expire - now) : 1; // 至少1ms

		struct itimerspec its;
		its.it_value.tv_sec = delay / 1000;
		its.it_value.tv_nsec = (delay % 1000) * 1000000;
		its.it_interval.tv_sec = 0; // 单次触发
		its.it_interval.tv_nsec = 0;

		timerfd_settime(timer_fd_, 0, &its, nullptr);
	}

	void processTimers() {
		constexpr int MAX_EVENTS = 32;
		struct epoll_event events[MAX_EVENTS];

		while (running_) {
			int timeout = -1;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (!heap_.empty()) {
					uint64_t now = getCurrentMs();
					uint64_t expire = heap_.front()->expire;
					// 无符号相减后再窄化成 int 可能得到负数（变成 1ms 忙等），这里先比较再截断
					timeout = expire <= now ? 1 : (int)std::min<uint64_t>(expire - now, INT_MAX);
				}
			}

			int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout);
			if (!running_) break;

			if (nfds == -1) {
				if (errno == EINTR) continue;
				perror("epoll_wait");
				break;
			}

			for (int i = 0; i < nfds; ++i) {
				// 处理唤醒事件
				if (events[i].data.fd == wakeup_fd_) {
					uint64_t dummy;
					read(wakeup_fd_, &dummy, sizeof(dummy));
					continue; // 纯唤醒，无业务逻辑
				}
				// 处理定时器事件...
				if (events[i].data.fd == timer_fd_) {
					uint64_t expirations;
					read(timer_fd_, &expirations, sizeof(expirations));
					processExpiredTimers();
				}
			}

			// epoll_wait 的毫秒超时可能先于 timerfd 到点：补偿检查一次
			if (nfds == 0) {
				processExpiredTimers();
			}
		}
	}

	void processExpiredTimers() {
		std::unique_lock<std::mutex> lock(mutex_);
		uint64_t now = getCurrentMs();

		while (!heap_.empty() && heap_.front()->expire <= now) {
			std::shared_ptr<TimerNode> timer = popHeap();

			if (!timer->valid) {
				--cancelled_in_heap_;
				continue; // 已被取消
			}

			// 执行回调（解锁避免死锁）
			lock.unlock();
			timer->cb(timer->arg);
			lock.lock();

			// 处理周期性定时器
			if (timer->periodic && timer->valid) {
				// 时间补偿逻辑
				uint64_t drift = now - timer->expire;
				timer->expire = now + timer->interval - std::min(drift, timer->interval);
				pushHeap(timer);
			}
			else {
				timers_.erase(timer->id);
			}
		}

		// 重置timerfd为下一个定时器
		resetTimerFd();
	}

	static uint64_t getCurrentMs() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
};
