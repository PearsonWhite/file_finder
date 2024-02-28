# Purpose of Design

This program was designed to demonstrate multithreading in C++ with worker threads and a UI thread. The UI thread allows interaction while work continues in the background and demonstrates how to communicate with worker threads in order to gracefully shut down threads without having to detatch threads. It also demonstrates how to detatch a thread with the UI thread. From this program, one should be able to learn how to create threads, manage tasks on other threads, and communicate between them all utilizing stl constructs.

Incidentally, this also demonstrates how to parse arguments and run commands utilizing std::variant as well as a basic logging mechanism for multithreaded programs.

# Program

```
Usage: ./file_finder.exe <dir> <substring1>[<substring2> [<substring3>]...]\n"
Traverses a directory tree and prints out any paths whose filenames contain the given substrings.
Example: file_finder.exe D:\\Documents\\Alice report book draft

Options:
--help           Output usage message and exit.
--test           Run tests.
<dir>            Root directory to begin traversing.
<substring1..n>  Substring to search for in file names.
```

- Each substring is processed on its own thread.
- Results are periodically dumped.
- Command `end`  ends the program
- or `dump` to dump what has been found since the last dump


# Design

The design is very simple. There is a `search_thread`, which traverses the file system from the specified root path. This uses the PathFinder class. The `path_finder` pushes these results to a set of processors. Each processor runs on its own thread and checks if its target substring appears in the filename of the entrys pushed into its queue. If the subtring does appear, it pushes a SearchResult to its SearchResultContainer. The dump_thread periodically dumps the contents of the SearchResultContainer. A ui_thread parses and executes commands. The main thread handles the creation, waiting, and end synchronization of all threads. When the command is given to end the program, `should_continue` is set to `false` for all threads, and we wait for them to return. If the `search_thread` finishes before the command to end the program is given, the main thread waits for the processors to finish before giving the command to end itself.<br/>

The main bottleneck will be traversing the filesystem. For this reason the processors are kept as open as possible. However, they are still locked during the actual processing where they find the target substring in the paths they've been given. This can be optimized by using a second queue and swapping them during processing. This way, there will always be a queue the can be pushed to, no matter how long it takes the processor to process the file entries it has been given. This optimization isn't nessesary now, but if the processors were on a longer delay, and performed more computationally demanding work, it may become a better option. Such an optimization could also be implemented for the ResultsContainer when it dumps its current results.<br/>

# Results

The results will be printed out in the following format:
```
path
"substring" (thread_id)
```
where `substring` is the given substring found in the path and `thread_id` is the processor thread that found the substring in the path.

If the program has found that a filename contains more than one substring [see note](#multiple-substrings-found), it will be printed out at the same time:
```
path
"substring 1" (thread_id)
"substring 2" (thread_id)
```

# Behavior

The program does not check if folders contain given substrings. This is intentional.<br/>
For example: `file-finder . foo` would find nothing with the following file system:
```
-Alice
-Bob
--projects
---bar
----main.cpp
---foo
```
where `foo` is a folder.<br/>

# Todos

1. Use a proper issue-tracking system.
2. Split code into reasonable segments and place is separate files. This would also require using a build system (or build system generator like CMake).
3. Use a proper testing framework (like Catch2).

## Lacking features

1. Symlinks, Shortcuts, etc. are not currently accounted for.<br/>
2. There is no option to find folders matching substring names.<br/>
3. There is no support for substrings with spaces. For example: `file-finder /Volumes/D/root "ab c" dd ee`<br/>
4. Unicode and filenames with special/unusual characters are not tested.<br/>
5. No option to ignore case.<br/>
6. No wildcard characters or regex.<br/>
7. Count how many files were found, how many were processed, etc.<br/>

# Considerations

## Multiple Substrings Found

The printed paths are not guaranteed to be printed only once. For example, you could have the following output:
```
D:\Alice\Documents\book_report_1.doc
"report" (2)
```
and later:
```
D:\Alice\Documents\book_report_1.doc
"book" (1)
```

This is because the ResultsContainer clears its store whenever it dumps. To change this, you would need to pass all paths to the ResultsContainer, so it knows when all processesors have seen the path. This works under the assumption that we will not traverse over the same path more than once.
