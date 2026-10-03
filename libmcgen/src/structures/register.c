/* structures/register.c — единственное место со списком модулей Java-кодированных построек (structure_register_all вызывает
 * structures_register_extra). Каждый модуль structures/<имя>.c определяет `void structures_register_<имя>(void)`; список фиксирован,
 * поэтому параллельные потоки работ правят только свои файлы. */
#include "../structure.h"

void structures_register_desert_pyramid(void);
void structures_register_jungle_temple(void);
void structures_register_swamp_hut(void);
void structures_register_igloo(void);
void structures_register_buried_treasure(void);
void structures_register_shipwreck(void);
void structures_register_ocean_ruin(void);
void structures_register_mineshaft(void);
void structures_register_stronghold(void);
void structures_register_ocean_monument(void);
void structures_register_woodland_mansion(void);
void structures_register_nether_fortress(void);
void structures_register_end_city(void);
void structures_register_ruined_portal(void);
void structures_register_nether_fossil(void);

void structures_register_extra(void) {
    structures_register_desert_pyramid();
    structures_register_jungle_temple();
    structures_register_swamp_hut();
    structures_register_igloo();
    structures_register_buried_treasure();
    structures_register_shipwreck();
    structures_register_ocean_ruin();
    structures_register_mineshaft();
    structures_register_stronghold();
    structures_register_ocean_monument();
    structures_register_woodland_mansion();
    structures_register_nether_fortress();
    structures_register_end_city();
    structures_register_ruined_portal();
    structures_register_nether_fossil();
}
