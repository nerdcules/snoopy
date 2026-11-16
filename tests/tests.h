#pragma once

// Each area keeps its Qt Test classes in its own file and runs them from
// here, so the core, the icon reader and the app can grow apart.
int runAppTests(int argc, char **argv);
int runReportTests(int argc, char **argv);
int runIconTests(int argc, char **argv);
int runWatchTests(int argc, char **argv);
