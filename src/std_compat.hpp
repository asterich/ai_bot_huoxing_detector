#pragma once

// GCC 16.1 can mismerge language linkage while loading partial libstdc++
// declarations from several third-party global module fragments. Prime those
// declarations consistently at the vendor boundary. Project code still imports
// the real toolchain std module; Clang needs no textual standard-library prelude.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 16 && __GNUC_MINOR__ == 1
#include <bits/stdc++.h>
#endif
