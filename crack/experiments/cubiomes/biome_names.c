// Печатает таблицу id -> имя биома (biome2str) для указанной версии cubiomes.
#include "biomes.h"
#include "util.h"
#include <stdio.h>
int main(int argc, char **argv)
{
    int mc = MC_1_21_WD;
    for (int id = 0; id < 256; id++)
    {
        const char *s = biome2str(mc, id);
        if (s && biomeExists(mc, id)) printf("%d %s\n", id, s);
    }
    return 0;
}
