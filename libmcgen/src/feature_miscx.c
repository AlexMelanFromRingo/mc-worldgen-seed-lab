/* feature_miscx.c — единая точка регистрации групп W12 (подземные, ледяные, особые, Nether, End). Сами фичи — в feature_nether.c,
 * feature_geode.c, feature_drip.c, feature_ice.c, feature_end.c, feature_misc_*.c. */
#include "feature_misc.h"

void feature_register_nether(void);
void feature_register_end(void);
void feature_register_ice(void);
void feature_register_misc_all(void) {
    feature_register_nether();
    feature_register_end();
    feature_register_ice();
}
