#include<iostream>
#include<queue>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <fstream>
#include <string>
#include <atomic>
#include <thread>
// ---------- 日志级别 ----------
enum class LogLevel { Debug, Info, Warn, Error };

static inline const char* to_cstr(LogLevel lv) {
    switch (lv) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}
// ---------- 日志记录结构 ----------
struct LogRecord {
    std::chrono::system_clock::time_point ts;
    LogLevel level;
    std::thread::id tid;
    std::string message;
};
//抽象类，用于后期进行接口拓展
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& rec) = 0;//用于处理一条日志消息的方法
};
// ---------- 简单格式化器 ----------
static inline std::string default_format(const LogRecord& r);
//将所有信息拼成一条日志
static inline std::string default_format(const LogRecord& r) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    char buffer[100];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&now_time));
    std::ostringstream oss;
    oss << buffer
        << " [" << to_cstr(r.level) << "]"
        << " (tid=" << r.tid << ") "
        << r.message;
    return oss.str();
}
// static inline LogRecord convert_To_Rec(std::string msg){

// }
//控制台输出观察者
class ConsoleSink :public LogSink {
public:

    void write(const LogRecord& rec) {

        std::string line = default_format(rec);
        //在控制台输出日志
        std::cout << line << '\n';
    }
};

//将其他类型变量转换为字符串类型的辅助函数
template<typename T>
std::string to_string_helper(T&& arg) {
    std::ostringstream oss;
    oss << std::forward<T>(arg);
    return oss.str();
}

//需要设计一个面向多线程的队列类，以实现日志类在子线程工作
class LogQueue {
public:
    LogQueue() = default;
    ~LogQueue() = default;
    //生产者
    void push(LogRecord msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(msg);
        cv_.notify_one();//通知消费者线程进行消费
    }


    //消费者
    //这里采用​​输出型参数​​模式，即使用形参msg作为函数的返回值，这样避免了多次数据拷贝带来的资源浪费

    bool pop(LogRecord& msg) {
        std::unique_lock<std::mutex> lock(mtx_);//要和条件变量配合使用因此用unique_lock
        cv_.wait(lock, [this]() {
            return !queue_.empty() || is_shutdown_;
            });//等待队列不为空再消费数据

        if (queue_.empty() && is_shutdown_) {
            return false;//队列退出逻辑，如果为空且队列已经关闭，那么返回false
        }
        msg = queue_.front();
        queue_.pop();
        return true;

    }
    //释放队列
    void shutdown() {
        std::lock_guard<std::mutex> lock(mtx_);
        is_shutdown_ = true;
        cv_.notify_all();
    }
private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<LogRecord> queue_;
    std::atomic<bool> is_shutdown_ = false;
};

class Logger {
public:
    //单例模式：外部调用的获取单例
    static Logger& getInstance() {
        static Logger l;
        return l;
    }
    //添加观察者
    void add_sink(std::shared_ptr<LogSink> s) {
        std::lock_guard<std::mutex> lk(sinks_mtx_);
        sinks_.push_back(std::move(s));//往观察者数组后面添加
    }
    //移除所有观察者
    void remove_all_sinks() {
        std::lock_guard<std::mutex> lk(sinks_mtx_);
        sinks_.clear();
    }
    //禁用左值引用/右值引用的拷贝构造
    Logger(const Logger& l) = delete;
    Logger(const Logger&& l) = delete;
    //禁用左值引用/右值引用的赋值
    Logger& operator= (const Logger& l) = delete;
    Logger& operator= (const Logger&& l) = delete;

    ~Logger() {
        log_queue_.shutdown();
        exit_flag_ = true;
        if (log_file_.is_open()) {
            log_file_.close();
        }
        if (worker_thread_.joinable()) { // 重要：检查线程是否可连接
            worker_thread_.join(); // 等待工作线程完成其任务并退出
        }
    }
    //核心日志记录函数
    template <typename ... Args>
    void log(LogLevel level, const std::string& format, Args&&... args) {
        std::string formatted_msg = format_Message(format, std::forward<Args>(args)...);
        // 创建完整的LogRecord对象
        LogRecord record;
        record.ts = std::chrono::system_clock::now();
        record.level = level;  // 使用传入的日志级别
        record.tid = std::this_thread::get_id();
        record.message = std::move(formatted_msg);
        //然后再压入队列
        log_queue_.push(record);
    }
    //便捷输出的记录函数
    // 便捷宏风格
    template <typename... A> void debug(const std::string& f, A&&... a) {
        log(LogLevel::Debug, f, std::forward<A>(a)...);
    }
    template <typename... A> void info(const std::string& f, A&&... a) { log(LogLevel::Info, f, std::forward<A>(a)...); }
    template <typename... A> void warn(const std::string& f, A&&... a) { log(LogLevel::Warn, f, std::forward<A>(a)...); }
    template <typename... A> void error(const std::string& f, A&&... a) { log(LogLevel::Error, f, std::forward<A>(a)...); }

private:
    //单例模式：私有构造函数
    //观察者模式，需要重构发布者的构造函数
    Logger() {
        worker_thread_ = std::thread([this] {
            LogRecord rec;//消息
            for (;;) {
                if (!log_queue_.pop(rec))
                    break;
                std::vector<std::shared_ptr<LogSink>> copy;
                //加一个作用域，用于lock_guard自动释放
                //减少拷贝一份观察者对象的加锁的时间
                {
                    std::lock_guard<std::mutex> lk(sinks_mtx_);
                    copy = sinks_; // 拷贝一份，减少锁持有时间
                }
                //广播给所有观察者
                for (auto& s : copy) {
                    if (s)
                        s->write(rec);//调用观察者的write
                }
            }
            });
    }


    LogQueue log_queue_;
    std::thread worker_thread_;//日志工作线程
    std::ofstream log_file_;//记录日志的文件
    std::atomic<bool> exit_flag_;//日志退出的标志
    //为什么要用智能指针操作观察者？
    std::vector<std::shared_ptr<LogSink>> sinks_;//观察者智能指针数组
    //用互斥锁保证广播的原子性
    std::mutex sinks_mtx_;

    //格式化字符串拼接
    template <typename ... Args>
    std::string format_Message(const std::string& format, Args&&...args) {
        //首先把args里面存的可变参数列表转换为字符串组成的vector
        //这一步首先是调用to_string_helper进行一个arg的转换，需要对使用forward将万能模板Args&&类型转换为右值
        //然后再使用...进行堆叠操作
        //最后用{}把转换完成的字符串全部拼接在一起
        std::vector<std::string> arg_strings = { to_string_helper(std::forward<Args>(args))... };
        std::ostringstream oss;//最后拼接成的总字符串，是函数的返回值
        size_t arg_idx = 0;//需要拼接的参数序号
        size_t pos = 0;//函数已经处理到的位置
        size_t place_holder = format.find("{}", pos);//第一个找到的{}位置
        //如果format里面包含{},那么进行后续的查找替换操作
        while (place_holder != std::string::npos) {
            oss << format.substr(pos, place_holder - pos);//把第一个{}之前的字符串先添加到oss里面
            if (arg_idx < arg_strings.size()) {
                //如果args里面有足够的参数进行{}替换
                oss << arg_strings[arg_idx++];//先把arg_strings对应idx的字符串拼进来，然后再对arg_idx++
            }
            else {
                oss << "{}";//如果args里面没有足够的参数进行{}替换，那么保留原来的{}
            }
            pos = place_holder + 2;//跳过"{}"，继续处理后面的
            place_holder = format.find("{}", pos);//更新place_holder

        }
        //如果已经匹配了所有的"{}",那么拼接后面的所有字符串
        oss << format.substr(pos);
        //还需要防范一种情况：参数args比{}数量多，那么需要把所有剩下的args拼到oss后面
        while (arg_idx < arg_strings.size()) {
            oss << arg_strings[arg_idx++];
        }
        return oss.str();
    }
};


//文件输出观察者
class FileSink :public LogSink {
public:
    explicit FileSink(const std::string& filename) :log_file_(std::ofstream(filename, std::ios::out | std::ios::app)) {
        if (!log_file_.is_open()) {
            throw  std::runtime_error("failed to open log file");
        }

    }
    void write(const LogRecord& rec) {

        std::string line = default_format(rec);
        //在网络输出日志
        log_file_ << line << '\n';
    }
private:
    std::ofstream log_file_;
};
//后续可以继续扩展日志输出的形
#pragma once
