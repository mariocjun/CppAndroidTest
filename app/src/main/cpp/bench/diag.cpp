#include "diag.h"

namespace bench::diag {
std::atomic<const char*> current_bench{"(idle)"};
}
