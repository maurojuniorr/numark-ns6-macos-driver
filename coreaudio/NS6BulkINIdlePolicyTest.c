#include "NS6BulkINIdlePolicy.h"

#include <assert.h>

int main(void) {
    assert(ns6_capture_demand_recent(true, 1000000000ull, 1200000000ull));
    assert(!ns6_capture_demand_recent(true, 1000000000ull, 1250000000ull));
    assert(!ns6_capture_demand_recent(false, 1000000000ull, 1100000000ull));
    assert(ns6_bulk_in_reads_should_run(false, false));
    assert(ns6_bulk_in_reads_should_run(true, true));
#if NS6_KEEP_WAVEFORM_DRAIN_IDLE
    assert(ns6_bulk_in_reads_should_run(true, false));
#else
    assert(!ns6_bulk_in_reads_should_run(true, false));
#endif
    return 0;
}
