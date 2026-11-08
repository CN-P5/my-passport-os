/* Host-test stub: weather parsing reports sunrise/sunset to the appearance
 * module, which is firmware-only state. Keep the dependency link-satisfied. */
#include "ui/ui_appearance.h"

void ui_appearance_set_sun_times(bool valid, uint16_t sunrise_min, uint16_t sunset_min)
{
    (void)valid;
    (void)sunrise_min;
    (void)sunset_min;
}
