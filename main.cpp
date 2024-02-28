#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <queue>
#include <ranges>
#include <string>
#include <syncstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

struct Logger {
  enum struct Level {
    Silent = 0U,
    Error = 100,
    Warning = 200,
    Info = 300,
    Debug = 400
  };
  Level logging_level;

  Logger(Level level) : logging_level(level) {}

  std::string get_prefix(Level level) {
    std::string prefix;
    if (level == Level::Silent) {
      return "[SILENT]";
    } else if (level == Level::Error) {
      return "[ERROR]";
    } else if (level == Level::Warning) {
      return "[WARNING]";
    } else if (level == Level::Info) {
      return "[INFO]";
    } else {
      return "[DEBUG]";
    }
  }

  void log(std::string message, Level level, bool newline = true,
           bool flush = false) {
    std::scoped_lock(logging_lock);
    std::string prefix = this->get_prefix(level);
    if (level <= logging_level) {
      if (newline) {
        std::osyncstream(std::cout) << prefix << " " << message << " ("
                                    << std::this_thread::get_id() << ")"
                                    << "\n";
      } else {
        std::osyncstream(std::cout) << prefix << " " << message << " ("
                                    << std::this_thread::get_id() << ")";
      }
      if (flush) {
        std::osyncstream(std::cout).flush();
      }
    }
  }

  void debug(std::string message, bool newline = true, bool flush = true) {
    log(message, Level::Debug, newline, flush);
  }

  void info(std::string message, bool newline = true, bool flush = true) {
    log(message, Level::Info, newline, flush);
  }

private:
  std::mutex logging_lock;
};
Logger logger{Logger::Level::Info};

struct SearchResult {
  SearchResult(fs::directory_entry entry, std::string substring,
               std::thread::id id)
      : entry(entry), substring(substring), id(id) {}

  fs::directory_entry entry;
  std::string substring;
  std::thread::id id;
};

struct SearchResultContainer {
  SearchResultContainer(){};
  void push(SearchResult result) {
    std::scoped_lock<std::mutex> lock(store_mutex);
    logger.debug(std::format("push \"{}\"", result.entry.path().string()));
    this->store[result.entry.path()].emplace_back(result.substring, result.id);
  }

  void dump() {
    std::scoped_lock<std::mutex> lock(store_mutex);
    logger.info("dump start", true, true);
    std::stringstream ss;
    for (auto &[key, values] : this->store) {
      ss << key << "\n";
      for (const ResultValue &result : values) {
        ss << "\t\"" << result.first << "\"\t(" << result.second << ")\n";
      }
    }
    this->store.clear();
    std::osyncstream(std::cout) << ss.str();
    std::osyncstream(std::cout).flush();
  }

  int periodic_dump(
      std::chrono::milliseconds ms,
      std::chrono::milliseconds resolution = std::chrono::milliseconds{80}) {
    this->should_continue = true;
    auto start = std::chrono::high_resolution_clock::now();
    while (this->should_continue) {
      auto finish = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double, std::milli> elapsed = finish - start;
      if (elapsed > ms) {
        this->dump();
        start = std::chrono::high_resolution_clock::now();
      }
      std::this_thread::sleep_for(resolution);
    }
    logger.debug("dump end");
    return 0;
  }

  std::atomic_bool should_continue = false;

protected:
  using ResultValue = std::pair<std::string, std::thread::id>;
  std::unordered_map<fs::path, std::vector<ResultValue>> store;
  std::mutex store_mutex;
};

struct Processor {
  Processor(SearchResultContainer *container, std::string search_string)
      : target(search_string), container(container) {}

  Processor(Processor &&processor)
      : target(std::move(processor.target)), queue(std::move(processor.queue)),
        container(processor.container) {}

  std::queue<fs::directory_entry> queue;
  SearchResultContainer *container;

  void push(fs::directory_entry entry) {
    std::scoped_lock<std::mutex> lock(queue_mutex);
    logger.debug(std::format("push {}", entry.path().string()));
    this->queue.push(entry);
  }

  size_t queue_size() {
    std::scoped_lock<std::mutex> lock(queue_mutex);
    return queue.size();
  }

  int run(std::chrono::milliseconds resolution = std::chrono::milliseconds{
              500}) {
    this->should_continue = true;
    logger.debug("processor start");
    while (this->should_continue) {
      logger.debug(std::format("proc size: {}", this->queue_size()));
      this->process();
      std::this_thread::sleep_for(std::chrono::milliseconds(resolution));
    }
    logger.debug("processor end");
    return 0;
  }

  void process() {
    std::scoped_lock<std::mutex> lock(queue_mutex);
    while (this->queue.size() > 0) {
      fs::directory_entry &entry = this->queue.front();
      logger.debug(std::format("processing entry: \"{0}\" vs \"{1}\"",
                               entry.path().filename().string(), this->target));
      if (entry.path().filename().string().find(this->target) !=
          std::string::npos) {
        logger.debug(std::format("found {}", entry.path().filename().string()));
        this->container->push(
            SearchResult(entry, target, std::this_thread::get_id()));
      }
      this->queue.pop();
    }
  }

  const std::string target;
  std::atomic_bool should_continue{false};

private:
  std::mutex queue_mutex;
};

struct PathFinder {
  int list_paths(std::filesystem::path path, std::vector<Processor> *processors,
                 std::filesystem::directory_options &&options) {
    logger.debug("find start");
    this->should_continue = true;
    for (const fs::directory_entry &entry :
         fs::recursive_directory_iterator(path, options)) {
      if (!this->should_continue) {
        logger.debug("end_find (stop)");
        return 1;
      }
      for (Processor &proc : *processors) {
        if (entry.is_directory() == false) { // Ignore folders.
          proc.push(entry);
        }
      }
    }

    logger.debug("find end");
    return 0;
  }

  std::atomic_bool should_continue = false;
};

struct SearchSettings {
  fs::path root_dir;         // Root directory to begin traversing from.
  bool follow_links = false; // todo: Flags for different kinds of links
                             // (hardlink, symlink, shortcut, etc)
  std::vector<std::string> substrings; // Substring to look for in filenames
};

struct ArgumentException : std::runtime_error {
  ArgumentException(std::string message)
      : std::runtime_error(message.c_str()) {}
};

// Ordinarily we would use a test framework.
// Since the problem specified no external libraries, we'll add it here.
struct TestCommand {};

struct HelpCommand {
  HelpCommand(std::string help_message) : message(help_message) {}
  HelpCommand(const HelpCommand &command) : message(command.message) {}
  std::string to_string() { return this->message; }
  std::string message;
};

struct ArgParser {
  std::string get_help_string(std::string exe_name = "file-finder") const {
    return std::format(
        "Usage: {0} <dir> <substring1>[<substring2> [<substring3>]...]\n"
        "Traverses a directory tree and prints out any paths whose "
        "filenames "
        "contain the given substrings.\n"
        R"(Example: {0} D:\\Documents\\Alice report book draft )"
        "\n"
        "Options\n"
        "--help           Output usage message and exit.\n"
        "--test           Run tests.\n"
        "<dir>            Root directory to begin traversing.\n"
        "<substring1..n>  Substring to search for in file names.",
        exe_name);
  }

  /// @brief Parses CLI arguments and returns SearchSettings (or other
  /// command as appropriate).
  /// @param args CLI arguments. The first argument is expected to be the
  /// executable name.
  /// @return If the second argument is --help or --test, returns the
  /// corresponding command. Otherwise, returns settings for search as
  /// derived from given arguments.
  std::variant<SearchSettings, TestCommand, HelpCommand>
  parse_args(const std::vector<std::string> &args) {
    if (args.size() == 2) {
      if (args[1] == "--help") {
        return HelpCommand{this->get_help_string(args[0])};
      } else if (args[1] == "--test") {
        return TestCommand{};
      }
    }

    if (args.size() < 3) {
      if (args.size() == 0) {
        throw ArgumentException(std::format("Invalid number of arguments.\n{}",
                                            this->get_help_string()));
      } else {
        throw ArgumentException(std::format("Invalid number of arguments.\n{}",
                                            this->get_help_string(args[0])));
      }
    }

    fs::path root = args[1];
    if (!fs::exists(root)) {
      throw ArgumentException(
          std::format("Root path doesn't exist! (\"{}\")", root.string()));
    }

    SearchSettings settings{};
    settings.root_dir = root;
    for (auto itr : std::views::iota(std::begin(args) + 2, std::end(args))) {
      settings.substrings.emplace_back(*itr);
    }

    return settings;
  }
};

int do_main(SearchSettings settings) {
  logger.debug("do_main");

  SearchResultContainer *container = new SearchResultContainer();

  auto dump_period = std::chrono::milliseconds(9500); // ms_delay between dumps
  std::function<int()> dump_func = [container, dump_period]() {
    return container->periodic_dump(dump_period);
  };
  std::packaged_task<int()> dump(dump_func);
  std::thread dump_thread(std::move(dump));

  std::vector<std::thread> processor_threads;
  std::vector<Processor> *processors = new std::vector<Processor>();
  uint32_t index = 0;
  for (std::string substring : settings.substrings) {
    processors->emplace_back(container, substring);
    std::function<int()> fun = [processors, index]() {
      return (*processors)[index].run();
    };
    std::thread processor_thread(std::move(fun));
    processor_threads.emplace_back(std::move(processor_thread));
    ++index;
  }

  PathFinder *path_finder = new PathFinder();
  std::function<int()> search_func = [path_finder, settings, processors]() {
    using DirOptions = fs::directory_options;
    return path_finder->list_paths(settings.root_dir, processors,
                                   (settings.follow_links
                                        ? DirOptions::follow_directory_symlink
                                        : DirOptions::none) |
                                       DirOptions::skip_permission_denied);
  };
  std::packaged_task<int()> search_task(search_func);
  std::future search_future = search_task.get_future();
  std::thread search_thread(std::move(search_task));

  std::atomic_bool should_continue = true;

  auto stop_func = [&should_continue, &path_finder, &processors, &container]() {
    logger.info("ending");
    should_continue = false;
    path_finder->should_continue = false;
    for (Processor &processor : *processors) {
      processor.should_continue = false;
    }
    container->should_continue = false;
  };

  // Wait until all threads have started.
  bool ready = false;
  while (!ready) {
    ready = should_continue;
    ready &= path_finder->should_continue;
    for (Processor &processor : *processors) {
      ready &= processor.should_continue;
    }
    ready &= container->should_continue;
  }

  std::thread ui_thread([&]() {
    while (should_continue) {
      std::string command;
      std::getline(std::cin, command);

      if (command == "end" || command == "Exit") {
        stop_func();
      } else if (command == "dump" || command == "Dump") {
        container->dump();
      } else {
        std::osyncstream(std::cout)
            << "unknown command \"" << command << "\"" << std::endl;
      }
    }
  });

  while (should_continue && !(search_future.wait_for(std::chrono::milliseconds(
                                  150)) == std::future_status::ready)) {
  }

  // Search thread finished, but we may still have some processing to do.
  while (should_continue && std::transform_reduce(
              processors->begin(), processors->end(), 0, std::plus<>{},
              [](Processor &proc) { return proc.queue_size(); }) > 0) {
    // There is at least one processor with items to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  container->dump();

  stop_func();
  search_thread.join();
  for (std::thread &thread : processor_threads) {
    thread.join();
  }
  dump_thread.join();
  ui_thread.detach();

  std::osyncstream(std::cout).flush();

  delete path_finder;
  delete processors;
  delete container;

  return EXIT_SUCCESS;
}

int do_tests(); // todo: Remove forward declaration when tests are split into
                // separate file.
struct ArgVisitor {
  int operator()(SearchSettings settings) { return do_main(settings); }
  int operator()(TestCommand _) { return do_tests(); }
  int operator()(HelpCommand help) {
    std::cout << help.to_string() << std::endl;
    return EXIT_SUCCESS;
  }
};

int main(int argc, char *argv[]) {
  ArgParser parser;
  try {
    std::variant<SearchSettings, TestCommand, HelpCommand> args =
        parser.parse_args({argv, argv + argc});
    return std::visit(ArgVisitor{}, args);
  } catch (const ArgumentException &exception) {
    std::cout << exception.what() << std::endl;
    return EXIT_FAILURE;
  }
}

#pragma region Tests

struct TestContainer : SearchResultContainer {
  std::unordered_map<fs::path, std::vector<ResultValue>> get_store() {
    return this->store;
  }
};

struct TestResult {

  TestResult(std::string name) : name(name) {}

  bool passed() { return this->errors.size() == 0; }

  std::string name;
  std::vector<std::string> errors;
};

TestResult test_logging_prefix() {
  TestResult result("test_logging_prefix");
  Logger logger{Logger::Level::Debug};
  std::map<Logger::Level, std::string> map{
      {Logger::Level::Silent, "[SILENT]"},   {Logger::Level::Error, "[ERROR]"},
      {Logger::Level::Warning, "[WARNING]"}, {Logger::Level::Info, "[INFO]"},
      {Logger::Level::Debug, "[DEBUG]"},
  };
  for (auto &[key, value] : map) {
    std::string prefix = logger.get_prefix(key);
    if (prefix != value) {
      result.errors.emplace_back(
          std::format("Expected '{}'. Found '{}'", value, prefix));
    }
  }
  return result;
}

void expect_argument_exception(TestResult &result,
                               const std::vector<std::string> &args) {
  ArgParser parser;
  try {
    parser.parse_args(args);
  } catch (ArgumentException &exception) {
    return;
  } catch (std::exception &e) {
    std::cout << "other exception" << e.what() << std::endl;
  }
  result.errors.emplace_back("Expected ArgumentException!");
}

TestResult test_no_args() {
  TestResult result("test_no_args");
  expect_argument_exception(result, {});
  return result;
}

TestResult test_too_few_args() {
  TestResult result("test_too_few_args");
  expect_argument_exception(result, {"exe_name"});
  expect_argument_exception(result, {"exe_name", "root_dir"});
  return result;
}

TestResult test_help() {
  TestResult result("test_help");
  ArgParser parser;
  auto args = parser.parse_args({"exe_name", "--help"});
  if (!std::holds_alternative<HelpCommand>(args)) {
    result.errors.emplace_back("Expected HelpCommand.");
    return result;
  }

  int help_return = std::visit(ArgVisitor{}, args);
  if (help_return != 0) {
    result.errors.emplace_back(std::format(
        "Expected help command to return 0. Instead found {}", help_return));
  }

  return result;
}

TestResult test_root_dne() {
  TestResult result{"test_root_dne"};
  ArgParser parser;

  try {
    parser.parse_args({"exe_name", "root_dne", "arg1"});
  } catch (ArgumentException &exception) {
    std::string expected_error = "Root path doesn't exist! (\"root_dne\")";
    if (exception.what() != expected_error) {
      result.errors.emplace_back(std::format(
          "Expected error message: '{}'\tFound error message: '{}'\t",
          expected_error, exception.what()));
    }
    return result;
  }
  result.errors.emplace_back("No exception thrown. Expected ArgumentException");
  return result;
}

TestResult test_processor_find() {
  TestResult result("test_processor_find");
  result.errors.emplace_back("Error: Not Implemented. todo: implement");
  return result;

  TestContainer container;
  Processor proc{&container, {"foo"}};

  // todo: Create a std::filesystem::directory_entry
  // "E:\\Alice\\Bob\\foo.txt". std::filesystem::directory_entry entry{...}
  // proc.push(entry);

  if (container.get_store().size() != 1) {
    result.errors.emplace_back(
        std::format("Expected exactly one result. Instead found: {}",
                    container.get_store().size()));
  }

  auto &[key, value] = *container.get_store().begin();
  if (key.string() != "E:\\Alice\\Bob\\foo.txt") {
    result.errors.emplace_back("Incorrect path was pushed into container.");
  }
  // todo: check value

  return result;
}

// todo: Add test for: "E:\alice\bob\foo.txt" doesn't match "alice" or "bob" but
// does match "foo".

// todo: Add test for: Only filenames. E:\alice\bob\foo (folder) shouldn't be
// counted. Note: This check is done in the finder, not the processor.

int do_tests() {
  std::vector<TestResult> results;
  std::cout << "running tests" << std::endl;
  for (auto fun : {test_logging_prefix, test_no_args, test_too_few_args,
                   test_root_dne, test_help, test_processor_find

       }) {
    results.emplace_back(fun());
  }

  // Some tests will have output. Give ourselves some space.
  std::cout << "\n\n---------------\n\ntests finished\n" << std::endl;
  size_t failures = 0;
  for (const TestResult &result : results) {
    size_t test_errors = result.errors.size();
    failures += test_errors;
    std::cout << std::format("{} : {}", result.name,
                             test_errors > 0 ? "Failed" : "Passed")
              << std::endl;
    if (test_errors > 0) {
      for (const std::string &error : result.errors) {
        std::cout << "\t" << error << std::endl;
      }
    }
  }

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#pragma endregion Tests