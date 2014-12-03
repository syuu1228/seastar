/*
 * Copyright 2014 Cloudius Systems
 */

#ifndef REACTOR_HH_
#define REACTOR_HH_

#include <memory>
#include <type_traits>
#include <libaio.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unordered_map>
#include <netinet/ip.h>
#include <cstring>
#include <cassert>
#include <stdexcept>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <thread>
#include <system_error>
#include <chrono>
#include <atomic>
#include <experimental/optional>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include "util/eclipse.hh"
#include "future.hh"
#include "posix.hh"
#include "apply.hh"
#include "sstring.hh"
#include "timer-set.hh"
#include "deleter.hh"
#include "net/api.hh"
#include "temporary_buffer.hh"
#include "circular_buffer.hh"
#include "file.hh"
#include "semaphore.hh"

#ifdef HAVE_OSV
#include <osv/newpoll.hh>
#endif

class reactor;
class pollable_fd;
class pollable_fd_state;

template <typename CharType>
class input_stream;

template <typename CharType>
class output_stream;

struct free_deleter {
    void operator()(void* p) { ::free(p); }
};

template <typename CharType>
inline
std::unique_ptr<CharType[], free_deleter> allocate_aligned_buffer(size_t size, size_t align) {
    static_assert(sizeof(CharType) == 1, "must allocate byte type");
    void* ret;
    auto r = posix_memalign(&ret, align, size);
    assert(r == 0);
    return std::unique_ptr<CharType[], free_deleter>(reinterpret_cast<CharType*>(ret));
}

using clock_type = std::chrono::high_resolution_clock;

class timer {
    using callback_t = std::function<void()>;
    boost::intrusive::list_member_hook<> _link;
    callback_t _callback;
    clock_type::time_point _expiry;
    boost::optional<clock_type::duration> _period;
    bool _armed = false;
    bool _queued = false;
    bool _expired = false;
public:
    ~timer();
    future<> expired();
    void set_callback(callback_t&& callback);
    void arm(clock_type::time_point until, boost::optional<clock_type::duration> period = {});
    void rearm(clock_type::time_point until, boost::optional<clock_type::duration> period = {});
    void arm(clock_type::duration delta);
    void arm_periodic(clock_type::duration delta);
    bool armed() const { return _armed; }
    bool cancel();
    clock_type::time_point get_timeout();
    friend class reactor;
    friend class timer_set<timer, &timer::_link, clock_type>;
};

class pollable_fd_state {
public:
    struct speculation {
        int events = 0;
        explicit speculation(int epoll_events_guessed = 0) : events(epoll_events_guessed) {}
    };
    ~pollable_fd_state();
    explicit pollable_fd_state(file_desc fd, speculation speculate = speculation())
        : fd(std::move(fd)), events_known(speculate.events) {}
    pollable_fd_state(const pollable_fd_state&) = delete;
    void operator=(const pollable_fd_state&) = delete;
    void speculate_epoll(int events) { events_known |= events; }
    file_desc fd;
    int events_requested = 0; // wanted by pollin/pollout promises
    int events_epoll = 0;     // installed in epoll
    int events_known = 0;     // returned from epoll
    promise<> pollin;
    promise<> pollout;
    friend class reactor;
    friend class pollable_fd;
};

class pollable_fd {
public:
    using speculation = pollable_fd_state::speculation;
    std::unique_ptr<pollable_fd_state> _s;
    pollable_fd(file_desc fd, speculation speculate = speculation())
        : _s(std::make_unique<pollable_fd_state>(std::move(fd), speculate)) {}
public:
    pollable_fd(pollable_fd&&) = default;
    pollable_fd& operator=(pollable_fd&&) = default;
    future<size_t> read_some(char* buffer, size_t size);
    future<size_t> read_some(uint8_t* buffer, size_t size);
    future<size_t> read_some(const std::vector<iovec>& iov);
    future<size_t> write_all(const char* buffer, size_t size);
    future<size_t> write_all(const uint8_t* buffer, size_t size);
    future<pollable_fd, socket_address> accept();
    future<size_t> sendmsg(struct msghdr *msg);
    future<size_t> recvmsg(struct msghdr *msg);
    future<size_t> sendto(socket_address addr, const void* buf, size_t len);
    file_desc& get_file_desc() const { return _s->fd; }
    void close() { _s.reset(); }
protected:
    int get_fd() const { return _s->fd.get(); }
    friend class reactor;
    friend class readable_eventfd;
    friend class writeable_eventfd;
};

class connected_socket_impl {
public:
    virtual ~connected_socket_impl() {}
    virtual input_stream<char> input() = 0;
    virtual output_stream<char> output() = 0;
};

class connected_socket {
    std::unique_ptr<connected_socket_impl> _csi;
public:
    explicit connected_socket(std::unique_ptr<connected_socket_impl> csi)
        : _csi(std::move(csi)) {}
    input_stream<char> input();
    output_stream<char> output();
};

class server_socket_impl {
public:
    virtual ~server_socket_impl() {}
    virtual future<connected_socket, socket_address> accept() = 0;
};

class client_socket_impl {
public:
    virtual ~client_socket_impl() {}
    virtual future<connected_socket> get_socket() = 0;
};


namespace std {

template <>
struct hash<::sockaddr_in> {
    size_t operator()(::sockaddr_in a) const {
        return a.sin_port ^ a.sin_addr.s_addr;
    }
};

}

bool operator==(const ::sockaddr_in a, const ::sockaddr_in b);

class server_socket {
    std::unique_ptr<server_socket_impl> _ssi;
public:
    explicit server_socket(std::unique_ptr<server_socket_impl> ssi)
        : _ssi(std::move(ssi)) {}
    future<connected_socket, socket_address> accept() {
        return _ssi->accept();
    }
};

class client_socket {
    std::unique_ptr<client_socket_impl> _csi;
public:
    explicit client_socket(std::unique_ptr<client_socket_impl> csi)
        : _csi(std::move(csi)) {}
    future<connected_socket> get_socket() {
        return _csi->get_socket();
    }
};

class network_stack {
public:
    virtual ~network_stack() {}
    virtual server_socket listen(socket_address sa, listen_options opts) = 0;
    virtual client_socket connect(socket_address sa) = 0;
    virtual net::udp_channel make_udp_channel(ipv4_addr addr = {}) = 0;
    virtual future<> initialize() {
        return make_ready_future();
    }
    virtual bool has_per_core_namespace() = 0;
};

class network_stack_registry {
public:
    using options = boost::program_options::variables_map;
private:
    static std::unordered_map<sstring,
            std::function<future<std::unique_ptr<network_stack>> (options opts)>>& _map() {
        static std::unordered_map<sstring,
                std::function<future<std::unique_ptr<network_stack>> (options opts)>> map;
        return map;
    }
    static sstring& _default() {
        static sstring def;
        return def;
    }
public:
    static boost::program_options::options_description& options_description() {
        static boost::program_options::options_description opts;
        return opts;
    }
    static void register_stack(sstring name,
            boost::program_options::options_description opts,
            std::function<future<std::unique_ptr<network_stack>> (options opts)> create,
            bool make_default = false);
    static sstring default_stack();
    static std::vector<sstring> list();
    static future<std::unique_ptr<network_stack>> create(options opts);
    static future<std::unique_ptr<network_stack>> create(sstring name, options opts);
};

class network_stack_registrator {
public:
    using options = boost::program_options::variables_map;
    explicit network_stack_registrator(sstring name,
            boost::program_options::options_description opts,
            std::function<future<std::unique_ptr<network_stack>> (options opts)> factory,
            bool make_default = false) {
        network_stack_registry::register_stack(name, opts, factory, make_default);
    }
};

class writeable_eventfd;

class readable_eventfd {
    pollable_fd _fd;
public:
    explicit readable_eventfd(size_t initial = 0) : _fd(try_create_eventfd(initial)) {}
    readable_eventfd(readable_eventfd&&) = default;
    writeable_eventfd write_side();
    future<size_t> wait();
    int get_write_fd() { return _fd.get_fd(); }
private:
    explicit readable_eventfd(file_desc&& fd) : _fd(std::move(fd)) {}
    static file_desc try_create_eventfd(size_t initial);

    friend class writeable_eventfd;
};

class writeable_eventfd {
    file_desc _fd;
public:
    explicit writeable_eventfd(size_t initial = 0) : _fd(try_create_eventfd(initial)) {}
    writeable_eventfd(writeable_eventfd&&) = default;
    readable_eventfd read_side();
    void signal(size_t nr);
    int get_read_fd() { return _fd.get(); }
private:
    explicit writeable_eventfd(file_desc&& fd) : _fd(std::move(fd)) {}
    static file_desc try_create_eventfd(size_t initial);

    friend class readable_eventfd;
};

// The reactor_notifier interface is a simplified version of Linux's eventfd
// interface (with semaphore behavior off, and signal() always signaling 1).
//
// A call to signal() causes an ongoing wait() to invoke its continuation.
// If no wait() is ongoing, the next wait() will continue immediately.
class reactor_notifier {
public:
    virtual future<> wait() = 0;
    virtual void signal() = 0;
    virtual ~reactor_notifier() {}
};

class thread_pool;
class smp;

class syscall_work_queue {
    static constexpr size_t queue_length = 128;
    struct work_item;
    using lf_queue = boost::lockfree::spsc_queue<work_item*,
                            boost::lockfree::capacity<queue_length>>;
    lf_queue _pending;
    lf_queue _completed;
    writeable_eventfd _start_eventfd;
    readable_eventfd _complete_eventfd;
    semaphore _queue_has_room = { queue_length };
    struct work_item {
        virtual ~work_item() {}
        virtual void process() = 0;
        virtual void complete() = 0;
    };
    template <typename T, typename Func>
    struct work_item_returning :  work_item {
        Func _func;
        promise<T> _promise;
        boost::optional<T> _result;
        work_item_returning(Func&& func) : _func(std::move(func)) {}
        virtual void process() override { _result = this->_func(); }
        virtual void complete() override { _promise.set_value(std::move(*_result)); }
        future<T> get_future() { return _promise.get_future(); }
    };
public:
    syscall_work_queue();
    template <typename T, typename Func>
    future<T> submit(Func func) {
        auto wi = new work_item_returning<T, Func>(std::move(func));
        auto fut = wi->get_future();
        submit_item(wi);
        return fut;
    }
    void start() { complete(); }
private:
    void work();
    void complete();
    void submit_item(work_item* wi);

    friend class thread_pool;
};

class smp_message_queue {
    static constexpr size_t queue_length = 128;
    struct work_item;
    using lf_queue = boost::lockfree::spsc_queue<work_item*,
                            boost::lockfree::capacity<queue_length>>;
    lf_queue _pending;
    lf_queue _completed;
    std::unique_ptr<reactor_notifier> _start_event;
    std::unique_ptr<reactor_notifier> _complete_event;
    size_t _current_queue_length = 0;
    reactor* _pending_peer;
    reactor* _complete_peer;
    struct work_item {
        virtual ~work_item() {}
        virtual future<> process() = 0;
        virtual void complete() = 0;
    };
    template <typename Func, typename Future>
    struct async_work_item : work_item {
        smp_message_queue& _q;
        Func _func;
        using value_type = typename Future::value_type;
        std::experimental::optional<value_type> _result;
        std::exception_ptr _ex; // if !_result
        typename Future::promise_type _promise; // used on local side
        async_work_item(smp_message_queue& q, Func&& func) : _q(q), _func(std::move(func)) {}
        virtual future<> process() override {
            try {
                return this->_func().rescue([this] (auto&& get_result) {
                    try {
                        _result = get_result();
                    } catch (...) {
                        _ex = std::current_exception();
                    }
                });
            } catch (...) {
                _ex = std::current_exception();
                return make_ready_future();
            }
        }
        virtual void complete() override {
            if (_result) {
                _promise.set_value(std::move(*_result));
            } else {
                // FIXME: _ex was allocated on another cpu
                _promise.set_exception(std::move(_ex));
            }
        }
        Future get_future() { return _promise.get_future(); }
    };
    std::queue<work_item*, circular_buffer<work_item*>> _pending_fifo;
public:
    smp_message_queue();
    template <typename Func>
    std::result_of_t<Func()> submit(Func func) {
        using future = std::result_of_t<Func()>;
        auto wi = new async_work_item<Func, future>(*this, std::move(func));
        auto fut = wi->get_future();
        submit_item(wi);
        return fut;
    }
    void start();
    void listen();
    size_t process_incoming();
    size_t process_completions();
private:
    void work();
    void complete();
    void submit_item(work_item* wi);
    void respond(work_item* wi);
    void submit_kick();
    void complete_kick();
    void move_pending();

    friend class smp;
};

class thread_pool {
#ifndef HAVE_OSV
    // FIXME: implement using reactor_notifier abstraction we used for SMP
    syscall_work_queue inter_thread_wq;
    posix_thread _worker_thread;
    std::atomic<bool> _stopped = { false };
public:
    thread_pool() : _worker_thread([this] { work(); }) { inter_thread_wq.start(); }
    ~thread_pool();
    template <typename T, typename Func>
    future<T> submit(Func func) {return inter_thread_wq.submit<T>(std::move(func));}
#else
public:
    template <typename T, typename Func>
    future<T> submit(Func func) { std::cout << "thread_pool not yet implemented on osv\n"; abort(); }
#endif
private:
    void work();
};

// The "reactor_backend" interface provides a method of waiting for various
// basic events on one thread. We have one implementation based on epoll and
// file-descriptors (reactor_backend_epoll) and one implementation based on
// OSv-specific file-descriptor-less mechanisms (reactor_backend_osv).
class reactor_backend {
public:
    virtual ~reactor_backend() {};
    // wait_and_process() waits for some events to become available, and
    // processes one or more of them. If block==false, it doesn't wait,
    // and just processes events that have already happened, if any.
    // After the optional wait, just before processing the events, the
    // pre_process() function is called.
    virtual void wait_and_process(bool block,
            std::function<void()> &&pre_process) = 0;
    // Methods that allow polling on file descriptors. This will only work on
    // reactor_backend_epoll. Other reactor_backend will probably abort if
    // they are called (which is fine if no file descriptors are waited on):
    virtual future<> readable(pollable_fd_state& fd) = 0;
    virtual future<> writeable(pollable_fd_state& fd) = 0;
    virtual void forget(pollable_fd_state& fd) = 0;
    // Methods that allow polling on a reactor_notifier. This is currently
    // used only for reactor_backend_osv, but in the future it should really
    // replace the above functions.
    virtual future<> notified(reactor_notifier *n) = 0;
    // Methods that allow capturing Unix signals.
    virtual future<> receive_signal(int signo) = 0;
    // Method for enabling a single timer (reactor multiplexes on this
    // multiple timers).
    virtual void enable_timer(clock_type::time_point when) = 0;
    virtual future<> timers_completed() = 0;
    // Methods for allowing sending notifications events between threads.
    virtual std::unique_ptr<reactor_notifier> make_reactor_notifier() = 0;
};

// reactor backend using file-descriptor & epoll, suitable for running on
// Linux. Can wait on multiple file descriptors, and converts other events
// (such as timers, signals, inter-thread notifications) into file descriptors
// using mechanisms like timerfd, signalfd and eventfd respectively.
class reactor_backend_epoll : public reactor_backend {
private:
    file_desc _epollfd;
    future<> get_epoll_future(pollable_fd_state& fd,
            promise<> pollable_fd_state::* pr, int event);
    void complete_epoll_event(pollable_fd_state& fd,
            promise<> pollable_fd_state::* pr, int events, int event);
    uint64_t _timers_completed;
    pollable_fd _timerfd = file_desc::timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    struct signal_handler {
        signal_handler(int signo);
        promise<> _promise;
        pollable_fd _signalfd;
        signalfd_siginfo _siginfo;
    };
    std::unordered_map<int, signal_handler> _signal_handlers;
public:
    reactor_backend_epoll();
    virtual ~reactor_backend_epoll() override { }
    virtual void wait_and_process(bool block, std::function<void()> &&pre_process) override;
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) override;
    virtual future<> notified(reactor_notifier *n) override;
    virtual future<> receive_signal(int signo) override;
    virtual void enable_timer(clock_type::time_point when) override;
    virtual future<> timers_completed() override;
    virtual std::unique_ptr<reactor_notifier> make_reactor_notifier() override;
};

#ifdef HAVE_OSV
// reactor_backend using OSv-specific features, without any file descriptors.
// This implementation cannot currently wait on file descriptors, but unlike
// reactor_backend_epoll it doesn't need file descriptors for waiting on a
// timer, for example, so file descriptors are not necessary.
class reactor_notifier_osv;
class reactor_backend_osv : public reactor_backend {
private:
    osv::newpoll::poller _poller;
    future<> get_poller_future(reactor_notifier_osv *n);
    promise<> _timer_promise;
public:
    reactor_backend_osv();
    virtual ~reactor_backend_osv() override { }
    virtual void wait_and_process(bool block,
            std::function<void()> &&pre_process) override;
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) override;
    virtual future<> notified(reactor_notifier *n) override;
    virtual future<> receive_signal(int signo) override;
    virtual void enable_timer(clock_type::time_point when) override;
    virtual future<> timers_completed() override;
    virtual std::unique_ptr<reactor_notifier> make_reactor_notifier() override;
    friend class reactor_notifier_osv;
};
#endif /* HAVE_OSV */

class reactor {
private:
    // FIXME: make _backend a unique_ptr<reactor_backend>, not a compile-time #ifdef.
#ifdef HAVE_OSV
    reactor_backend_osv _backend;
#else
    reactor_backend_epoll _backend;
#endif
    std::vector<std::function<bool()>> _pollers;
    static constexpr size_t max_aio = 128;
    promise<> _exit_promise;
    future<> _exit_future;
    std::atomic<bool> _idle;
    unsigned _id = 0;
    bool _stopped = false;
    bool _handle_sigint = true;
    bool _poll = false;
    promise<std::unique_ptr<network_stack>> _network_stack_ready_promise;
    std::unique_ptr<network_stack> _network_stack;
    int _return = 0;
    promise<> _start_promise;
    uint64_t _timers_completed;
    uint64_t _tasks_processed = 0;
    timer_set<timer, &timer::_link, clock_type> _timers;
    timer_set<timer, &timer::_link, clock_type>::timer_list_t _expired_timers;
    readable_eventfd _io_eventfd;
    io_context_t _io_context;
    semaphore _io_context_available;
    circular_buffer<std::unique_ptr<task>> _pending_tasks;
    thread_pool _thread_pool;
    size_t _task_quota;
private:
    void abort_on_error(int ret);
    void complete_timers();

    /**
     * Returns TRUE if all pollers allow blocking.
     *
     * @return FALSE if at least one of the blockers requires a non-blocking
     *         execution.
     */
    bool poll_once() {
        bool work = false;
        for (auto c : _pollers) {
            work |= c();
        }

        return work;
    }

public:
    static boost::program_options::options_description get_options_description();
    reactor();
    reactor(const reactor&) = delete;
    void operator=(const reactor&) = delete;

    void configure(boost::program_options::variables_map config);

    server_socket listen(socket_address sa, listen_options opts = {});

    client_socket connect(socket_address sa);

    pollable_fd posix_listen(socket_address sa, listen_options opts = {});

    pollable_fd posix_connect(socket_address sa);

    future<pollable_fd, socket_address> accept(pollable_fd_state& listen_fd);

    future<size_t> read_some(pollable_fd_state& fd, void* buffer, size_t size);
    future<size_t> read_some(pollable_fd_state& fd, const std::vector<iovec>& iov);

    future<size_t> write_some(pollable_fd_state& fd, const void* buffer, size_t size);

    future<size_t> write_all(pollable_fd_state& fd, const void* buffer, size_t size);

    future<file> open_file_dma(sstring name);

    template <typename Func>
    future<io_event> submit_io(Func prepare_io);

    int run();
    void exit(int ret);
    future<> when_started() { return _start_promise.get_future(); }

    template <typename Func>
    void at_exit(Func&& func) {
        _exit_future = _exit_future.then(func);
    }

    void add_task(std::unique_ptr<task>&& t) { _pending_tasks.push_back(std::move(t)); }

    network_stack& net() { return *_network_stack; }
    unsigned cpu_id() const { return _id; }
    bool idle() {
        if (_poll) {
            return false;
        } else {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return _idle.load(std::memory_order_relaxed);
        }
    }

    /**
     * Add a new "poller" - a non-blocking function returning a boolean, that
     * will be called every iteration of a main loop.
     * If it returns FALSE then reactor's main loop is forbidden to block in the
     * current iteration.
     *
     * @param fn a new "poller" function to register
     */
    void register_new_poller(std::function<bool()>&& fn) {
        _pollers.push_back(std::move(fn));
    }
private:
    struct collectd_registrations;
    collectd_registrations register_collectd_metrics();
    future<size_t> write_all_part(pollable_fd_state& fd, const void* buffer, size_t size, size_t completed);

    void process_io(size_t count);

    void add_timer(timer* tmr);
    void del_timer(timer* tmr);

    future<> run_exit_tasks();
    void stop();
    friend class pollable_fd;
    friend class pollable_fd_state;
    friend class posix_file_impl;
    friend class blockdev_file_impl;
    friend class readable_eventfd;
    friend class timer;
    friend class smp;
public:
    void wait_and_process(bool block, std::function<void()> &&pre_process) {
        _backend.wait_and_process(block, std::move(pre_process));
    }

    future<> readable(pollable_fd_state& fd) {
        return _backend.readable(fd);
    }
    future<> writeable(pollable_fd_state& fd) {
        return _backend.writeable(fd);
    }
    void forget(pollable_fd_state& fd) {
        _backend.forget(fd);
    }
    future<> notified(reactor_notifier *n) {
        return _backend.notified(n);
    }
    future<> receive_signal(int signo) {
        return _backend.receive_signal(signo);
    }
    void enable_timer(clock_type::time_point when) {
        _backend.enable_timer(when);
    }
    future<> timers_completed() {
        return _backend.timers_completed();
    }
    std::unique_ptr<reactor_notifier> make_reactor_notifier() {
        return _backend.make_reactor_notifier();
    }
};

extern thread_local reactor engine;
extern __thread size_t task_quota;

class smp {
	static std::vector<posix_thread> _threads;
	static smp_message_queue** _qs;
	static std::thread::id _tmain;

	template <typename Func>
    using returns_future = is_future<std::result_of_t<Func()>>;
    template <typename Func>
    using returns_void = std::is_same<std::result_of_t<Func()>, void>;
public:
	static boost::program_options::options_description get_options_description();
	static void configure(boost::program_options::variables_map vm);
	static void join_all();
	static bool main_thread() { return std::this_thread::get_id() == _tmain; }

	template <typename Func>
	static std::result_of_t<Func()> submit_to(unsigned t, Func func,
	        std::enable_if_t<returns_future<Func>::value, void*> = nullptr) {
	    if (t == engine.cpu_id()) {
	        return func();
	    } else {
	        return _qs[t][engine.cpu_id()].submit(std::move(func));
	    }
	}
    template <typename Func>
    static future<std::result_of_t<Func()>> submit_to(unsigned t, Func func,
            std::enable_if_t<!returns_future<Func>::value && !returns_void<Func>::value, void*> = nullptr) {
        return submit_to(t, [func = std::move(func)] () mutable {
           return make_ready_future<std::result_of_t<Func()>>(func());
        });
    }
    template <typename Func>
    static future<> submit_to(unsigned t, Func func,
            std::enable_if_t<!returns_future<Func>::value && returns_void<Func>::value, void*> = nullptr) {
        return submit_to(t, [func = std::move(func)] () mutable {
            func();
            return make_ready_future<>();
        });
    }
	static bool poll_queues() {
	    size_t got = 0;
	    for (unsigned i = 0; i < count; i++) {
	        if (engine.cpu_id() != i) {
                    got += _qs[engine.cpu_id()][i].process_incoming();
                    got += _qs[i][engine._id].process_completions();
	        }
	    }
	    return got != 0;
	}
private:
	static void listen_all(smp_message_queue* qs);
	static void start_all_queues();
public:
	static unsigned count;
};

inline
pollable_fd_state::~pollable_fd_state() {
    engine.forget(*this);
}

class data_source_impl {
public:
    virtual ~data_source_impl() {}
    virtual future<temporary_buffer<char>> get() = 0;
};

class data_source {
    std::unique_ptr<data_source_impl> _dsi;
public:
    explicit data_source(std::unique_ptr<data_source_impl> dsi) : _dsi(std::move(dsi)) {}
    data_source(data_source&& x) = default;
    future<temporary_buffer<char>> get() { return _dsi->get(); }
};

class data_sink_impl {
public:
    virtual ~data_sink_impl() {}
    virtual future<> put(std::vector<temporary_buffer<char>> data) = 0;
    virtual future<> put(temporary_buffer<char> data) {
        std::vector<temporary_buffer<char>> v;
        v.reserve(1);
        v.push_back(std::move(data));
        return put(std::move(v));
    }
    virtual future<> close() = 0;
};

class data_sink {
    std::unique_ptr<data_sink_impl> _dsi;
public:
    explicit data_sink(std::unique_ptr<data_sink_impl> dsi) : _dsi(std::move(dsi)) {}
    data_sink(data_sink&& x) = default;
    future<> put(std::vector<temporary_buffer<char>> data) {
        return _dsi->put(std::move(data));
    }
    future<> put(temporary_buffer<char> data) {
        return _dsi->put(std::move(data));
    }
    future<> close() { return _dsi->close(); }
};

template <typename CharType>
class input_stream {
    static_assert(sizeof(CharType) == 1, "must buffer stream of bytes");
    data_source _fd;
    temporary_buffer<CharType> _buf;
    bool _eof = false;
private:
    using tmp_buf = temporary_buffer<CharType>;
    size_t available() const { return _buf.size(); }
public:
    // Consumer concept, for consume() method:
    struct ConsumerConcept {
        // call done(tmp_buf) to signal end of processing. tmp_buf parameter to
        // done is unconsumed data
        template <typename Done>
        void operator()(tmp_buf data, Done done);
    };
    using char_type = CharType;
    explicit input_stream(data_source fd, size_t buf_size = 8192) : _fd(std::move(fd)), _buf(0) {}
    future<temporary_buffer<CharType>> read_exactly(size_t n);
    template <typename Consumer>
    future<> consume(Consumer& c);
    bool eof() { return _eof; }
private:
    future<temporary_buffer<CharType>> read_exactly_part(size_t n, tmp_buf buf, size_t completed);
};

template <typename CharType>
class output_stream {
    static_assert(sizeof(CharType) == 1, "must buffer stream of bytes");
    data_sink _fd;
    temporary_buffer<CharType> _buf;
    size_t _size;
    size_t _begin = 0;
    size_t _end = 0;
private:
    size_t available() const { return _end - _begin; }
    size_t possibly_available() const { return _size - _begin; }
public:
    using char_type = CharType;
    output_stream(data_sink fd, size_t size)
        : _fd(std::move(fd)), _buf(size), _size(size) {}
    future<> write(const char_type* buf, size_t n);
    future<> write(const char_type* buf);
    future<> write(const sstring& s);
    future<> flush();
    future<> close() { return _fd.close(); }
private:
};

template<typename CharType>
inline
future<> output_stream<CharType>::write(const char_type* buf) {
    return write(buf, strlen(buf));
}

template<typename CharType>
inline
future<> output_stream<CharType>::write(const sstring& s) {
    return write(s.c_str(), s.size());
}

inline
size_t iovec_len(const std::vector<iovec>& iov)
{
    size_t ret = 0;
    for (auto&& e : iov) {
        ret += e.iov_len;
    }
    return ret;
}

inline
size_t iovec_len(const iovec* begin, size_t len)
{
    size_t ret = 0;
    auto end = begin + len;
    while (begin != end) {
        ret += begin++->iov_len;
    }
    return ret;
}

inline
future<pollable_fd, socket_address>
reactor::accept(pollable_fd_state& listenfd) {
    return readable(listenfd).then([this, &listenfd] () mutable {
        socket_address sa;
        socklen_t sl = sizeof(&sa.u.sas);
        file_desc fd = listenfd.fd.accept(sa.u.sa, sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        pollable_fd pfd(std::move(fd), pollable_fd::speculation(EPOLLOUT));
        return make_ready_future<pollable_fd, socket_address>(std::move(pfd), std::move(sa));
    });
}

inline
future<size_t>
reactor::read_some(pollable_fd_state& fd, void* buffer, size_t len) {
    return readable(fd).then([this, &fd, buffer, len] () mutable {
        auto r = fd.fd.read(buffer, len);
        if (!r) {
            return read_some(fd, buffer, len);
        }
        if (size_t(*r) == len) {
            fd.speculate_epoll(EPOLLIN);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<size_t>
reactor::read_some(pollable_fd_state& fd, const std::vector<iovec>& iov) {
    return readable(fd).then([this, &fd, iov = iov] () mutable {
        ::msghdr mh = {};
        mh.msg_iov = &iov[0];
        mh.msg_iovlen = iov.size();
        auto r = fd.fd.recvmsg(&mh, 0);
        if (!r) {
            return read_some(fd, iov);
        }
        if (size_t(*r) == iovec_len(iov)) {
            fd.speculate_epoll(EPOLLIN);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<size_t>
reactor::write_some(pollable_fd_state& fd, const void* buffer, size_t len) {
    return writeable(fd).then([this, &fd, buffer, len] () mutable {
        auto r = fd.fd.send(buffer, len, MSG_NOSIGNAL);
        if (!r) {
            return write_some(fd, buffer, len);
        }
        if (size_t(*r) == len) {
            fd.speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<size_t>
reactor::write_all_part(pollable_fd_state& fd, const void* buffer, size_t len, size_t completed) {
    if (completed == len) {
        return make_ready_future<size_t>(completed);
    } else {
        return write_some(fd, static_cast<const char*>(buffer) + completed, len - completed).then(
                [&fd, buffer, len, completed, this] (size_t part) mutable {
            return write_all_part(fd, buffer, len, completed + part);
        });
    }
}

inline
future<size_t>
reactor::write_all(pollable_fd_state& fd, const void* buffer, size_t len) {
    assert(len);
    return write_all_part(fd, buffer, len, 0);
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read_exactly_part(size_t n, tmp_buf out, size_t completed) {
    if (available()) {
        auto now = std::min(n - completed, available());
        std::copy(_buf.get(), _buf.get() + now, out.get_write() + completed);
        _buf.trim_front(now);
        completed += now;
    }
    if (completed == n) {
        return make_ready_future<tmp_buf>(std::move(out));
    }

    // _buf is now empty
    return _fd.get().then([this, n, out = std::move(out), completed] (auto buf) mutable {
        if (buf.size() == 0) {
            return make_ready_future<tmp_buf>(std::move(buf));
        }
        _buf = std::move(buf);
        return this->read_exactly_part(n, std::move(out), completed);
    });
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read_exactly(size_t n) {
    if (_buf.size() == n) {
        // easy case: steal buffer, return to caller
        return make_ready_future<tmp_buf>(std::move(_buf));
    } else if (_buf.size() > n) {
        // buffer large enough, share it with caller
        auto front = _buf.share(0, n);
        _buf.trim_front(n);
        return make_ready_future<tmp_buf>(std::move(front));
    } else if (_buf.size() == 0) {
        // buffer is empty: grab one and retry
        return _fd.get().then([this, n] (auto buf) mutable {
            if (buf.size() == 0) {
                return make_ready_future<tmp_buf>(std::move(buf));
            }
            _buf = std::move(buf);
            return this->read_exactly(n);
        });
    } else {
        // buffer too small: start copy/read loop
        tmp_buf b(n);
        return read_exactly_part(n, std::move(b), 0);
    }
}

template <typename CharType>
template <typename Consumer>
future<>
input_stream<CharType>::consume(Consumer& consumer) {
    if (_buf.empty() && !_eof) {
        return _fd.get().then([this, &consumer] (tmp_buf buf) {
            _buf = std::move(buf);
            _eof = _buf.empty();
            return consume(consumer);
        });
    } else {
        auto tmp = std::move(_buf);
        bool done = tmp.empty();
        consumer(std::move(tmp), [this, &done] (tmp_buf unconsumed) {
            done = true;
            if (!unconsumed.empty()) {
                _buf = std::move(unconsumed);
            }
        });
        if (!done) {
            return consume(consumer);
        } else {
            return make_ready_future<>();
        }
    }
}

#include <iostream>
#include "sstring.hh"

template <typename CharType>
future<>
output_stream<CharType>::write(const char_type* buf, size_t n) {
    auto bulk_threshold = _end ? (2 * _size - _end) : _size;
    if (n >= bulk_threshold) {
        if (_end) {
            auto now = _size - _end;
            std::copy(buf, buf + now, _buf.get_write() + _end);
            temporary_buffer<char> tmp(n - now);
            std::copy(buf + now, buf + n, tmp.get_write());
            return flush().then([this, tmp = std::move(tmp)]() mutable {
                return _fd.put(std::move(tmp));
            });
        } else {
            temporary_buffer<char> tmp(n);
            std::copy(buf, buf + n, tmp.get_write());
            return _fd.put(std::move(tmp));
        }
    }
    auto now = std::min(n, _size - _end);
    std::copy(buf, buf + now, _buf.get_write() + _end);
    _end += now;
    if (now == n) {
        return make_ready_future<>();
    } else {
        temporary_buffer<CharType> next(_size);
        std::copy(buf + now, buf + n, next.get_write());
        _end = n - now;
        std::swap(next, _buf);
        return _fd.put(std::move(next));
    }
}

template <typename CharType>
future<>
output_stream<CharType>::flush() {
    if (!_end) {
        return make_ready_future<>();
    }
    _buf.trim(_end);
    temporary_buffer<CharType> next(_size);
    std::swap(_buf, next);
    _end = 0;
    return _fd.put(std::move(next));
}

inline
future<size_t> pollable_fd::read_some(char* buffer, size_t size) {
    return engine.read_some(*_s, buffer, size);
}

inline
future<size_t> pollable_fd::read_some(uint8_t* buffer, size_t size) {
    return engine.read_some(*_s, buffer, size);
}

inline
future<size_t> pollable_fd::read_some(const std::vector<iovec>& iov) {
    return engine.read_some(*_s, iov);
}

inline
future<size_t> pollable_fd::write_all(const char* buffer, size_t size) {
    return engine.write_all(*_s, buffer, size);
}

inline
future<size_t> pollable_fd::write_all(const uint8_t* buffer, size_t size) {
    return engine.write_all(*_s, buffer, size);
}

inline
future<pollable_fd, socket_address> pollable_fd::accept() {
    return engine.accept(*_s);
}

inline
future<size_t> pollable_fd::recvmsg(struct msghdr *msg) {
    return engine.readable(*_s).then([this, msg] {
        auto r = get_file_desc().recvmsg(msg, 0);
        if (!r) {
            return recvmsg(msg);
        }
        // We always speculate here to optimize for throughput in a workload
        // with multiple outstanding requests. This way the caller can consume
        // all messages without resorting to epoll. However this adds extra
        // recvmsg() call when we hit the empty queue condition, so it may
        // hurt request-response workload in which the queue is empty when we
        // initially enter recvmsg(). If that turns out to be a problem, we can
        // improve speculation by using recvmmsg().
        _s->speculate_epoll(EPOLLIN);
        return make_ready_future<size_t>(*r);
    });
};

inline
future<size_t> pollable_fd::sendmsg(struct msghdr* msg) {
    return engine.writeable(*_s).then([this, msg] () mutable {
        auto r = get_file_desc().sendmsg(msg, 0);
        if (!r) {
            return sendmsg(msg);
        }
        // For UDP this will always speculate. We can't know if there's room
        // or not, but most of the time there should be so the cost of mis-
        // speculation is amortized.
        if (size_t(*r) == iovec_len(msg->msg_iov, msg->msg_iovlen)) {
            _s->speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<size_t> pollable_fd::sendto(socket_address addr, const void* buf, size_t len) {
    return engine.writeable(*_s).then([this, buf, len, addr] () mutable {
        auto r = get_file_desc().sendto(addr, buf, len, 0);
        if (!r) {
            return sendto(std::move(addr), buf, len);
        }
        // See the comment about speculation in sendmsg().
        if (size_t(*r) == len) {
            _s->speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
timer::~timer() {
    if (_queued) {
        engine.del_timer(this);
    }
}

inline
void timer::set_callback(callback_t&& callback) {
    _callback = std::move(callback);
}

inline
void timer::arm(clock_type::time_point until, boost::optional<clock_type::duration> period) {
    assert(!_armed);
    _period = period;
    _armed = true;
    _expired = false;
    _expiry = until;
    engine.add_timer(this);
    _queued = true;
}

inline
void timer::rearm(clock_type::time_point until, boost::optional<clock_type::duration> period) {
    if (_armed) {
        cancel();
    }
    arm(until, period);
}

inline
void timer::arm(clock_type::duration delta) {
    return arm(clock_type::now() + delta);
}

inline
void timer::arm_periodic(clock_type::duration delta) {
    arm(clock_type::now() + delta, {delta});
}

inline
bool timer::cancel() {
    if (!_armed) {
        return false;
    }
    _armed = false;
    if (_queued) {
        engine.del_timer(this);
        _queued = false;
    }
    return true;
}

inline
clock_type::time_point timer::get_timeout() {
    return _expiry;
}

inline
input_stream<char>
connected_socket::input() {
    return _csi->input();
}

inline
output_stream<char>
connected_socket::output() {
    return _csi->output();
}

#endif /* REACTOR_HH_ */
