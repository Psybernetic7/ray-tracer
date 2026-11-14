#ifndef ANALYZE_H
#define ANALYZE_H

// `rt-cli analyze ...`: sun-hours and viewshed analysis of the generated terrain. argv[1] is
// the word "analyze"; options start at argv[2]. Returns the process exit code.
int run_analyze(int argc, char* argv[]);

#endif
