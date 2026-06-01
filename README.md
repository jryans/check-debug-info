## check-debug-info

The `check-debug-info` tool uses symbolic execution (via KLEE) to check the
correctness of local variable debug info. Two program variants (typically
unoptimised and optimised) are compared in this differential manner to find
debug info handling bugs during compiler optimisation.

More detail will be available in the future as part of my PhD thesis.

The source code for this tool is primarily located in the
[tools/check-debug-info](tools/check-debug-info) directory, though some core
KLEE execution files have modified as well to add additional symbolic execution
features to support the tool.
