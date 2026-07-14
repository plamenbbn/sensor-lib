#include "instrument_api.h"

#include "link_callback_harness.h"

#include <stdio.h>

int main(void) {
    const InstrumentAPIConfig config = {0};
    InstrumentAPI* const api = createInstrumentAPI(config);
    if (api == NULL) {
        fprintf(stderr, "Failed to create InstrumentAPI\n");
        return 1;
    }

    const int rc = run_link_callback_harness(api, 2U, HARNESS_WIFI_MODE_CLIENT);
    destroyInstrumentAPI(api);
    return rc;
}
