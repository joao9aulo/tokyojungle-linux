/* SPU workload registry for Tokyo Jungle (JP disc BCAS20219) -- regenerated.
 *
 * Each image was captured at runtime with SPU_DUMP_MISS and lifted with
 * scripts/tj_lift_spu.py. The runtime matches a guest SPU image by FNV-1a-64
 * fingerprint and runs the lifted entry registered here.
 */
#include "spu_workload.h"

extern void spu_begin_image(int image_id);

extern void tj_socjobspu_func_00000000(spu_context*);
extern void tj_socjobspu_recomp_register(void);
extern void tj_dspjobspu_func_00000000(spu_context*);
extern void tj_dspjobspu_recomp_register(void);
extern void tj_j1b6bdac3spu_func_00000000(spu_context*);
extern void tj_j1b6bdac3spu_recomp_register(void);
extern void tj_j48af2bc7spu_func_00000000(spu_context*);
extern void tj_j48af2bc7spu_recomp_register(void);
extern void tj_j49c1d6c4spu_func_00000000(spu_context*);
extern void tj_j49c1d6c4spu_recomp_register(void);
extern void tj_j5da19516spu_func_00000000(spu_context*);
extern void tj_j5da19516spu_recomp_register(void);
extern void tj_j80435d36spu_func_00000000(spu_context*);
extern void tj_j80435d36spu_recomp_register(void);
extern void tj_j82d0be14spu_func_00000000(spu_context*);
extern void tj_j82d0be14spu_recomp_register(void);
extern void tj_j9804aba8spu_func_00000000(spu_context*);
extern void tj_j9804aba8spu_recomp_register(void);
extern void tj_jabd49fc5spu_func_00000000(spu_context*);
extern void tj_jabd49fc5spu_recomp_register(void);
extern void tj_jd58063cdspu_func_00000000(spu_context*);
extern void tj_jd58063cdspu_recomp_register(void);
extern void tj_jfe031af6spu_func_00000000(spu_context*);
extern void tj_jfe031af6spu_recomp_register(void);
extern void tj_j78fea84espu_func_00000000(spu_context*);
extern void tj_j78fea84espu_recomp_register(void);
extern void tj_jd8945a0bspu_func_00000000(spu_context*);
extern void tj_jd8945a0bspu_recomp_register(void);
extern void tj_jdd6f7bfdspu_func_00000000(spu_context*);
extern void tj_jdd6f7bfdspu_recomp_register(void);

void tj_spu_register_all(void)
{
    spu_begin_image(1);
    tj_socjobspu_recomp_register();
    spu_workload_register_img(0xF523F26F69764538ULL, tj_socjobspu_func_00000000, 1, "socjob");

    spu_begin_image(2);
    tj_dspjobspu_recomp_register();
    spu_workload_register_img(0xFDFFAAC1BCD5352EULL, tj_dspjobspu_func_00000000, 2, "dspjob");

    spu_begin_image(3);
    tj_j1b6bdac3spu_recomp_register();
    spu_workload_register_img(0x1B6BDAC375C5438DULL, tj_j1b6bdac3spu_func_00000000, 3, "j1b6bdac3");

    spu_begin_image(4);
    tj_j48af2bc7spu_recomp_register();
    spu_workload_register_img(0x48AF2BC79A72CB50ULL, tj_j48af2bc7spu_func_00000000, 4, "j48af2bc7");

    spu_begin_image(5);
    tj_j49c1d6c4spu_recomp_register();
    spu_workload_register_img(0x49C1D6C47E80AF43ULL, tj_j49c1d6c4spu_func_00000000, 5, "j49c1d6c4");

    spu_begin_image(6);
    tj_j5da19516spu_recomp_register();
    spu_workload_register_img(0x5DA195169669A5FFULL, tj_j5da19516spu_func_00000000, 6, "j5da19516");

    spu_begin_image(7);
    tj_j80435d36spu_recomp_register();
    spu_workload_register_img(0x80435D36E623FAA0ULL, tj_j80435d36spu_func_00000000, 7, "j80435d36");

    spu_begin_image(8);
    tj_j82d0be14spu_recomp_register();
    spu_workload_register_img(0x82D0BE145EC28AEEULL, tj_j82d0be14spu_func_00000000, 8, "j82d0be14");

    spu_begin_image(9);
    tj_j9804aba8spu_recomp_register();
    spu_workload_register_img(0x9804ABA84566F431ULL, tj_j9804aba8spu_func_00000000, 9, "j9804aba8");

    spu_begin_image(10);
    tj_jabd49fc5spu_recomp_register();
    spu_workload_register_img(0xABD49FC58F53F368ULL, tj_jabd49fc5spu_func_00000000, 10, "jabd49fc5");

    spu_begin_image(11);
    tj_jd58063cdspu_recomp_register();
    spu_workload_register_img(0xD58063CD2A0236B5ULL, tj_jd58063cdspu_func_00000000, 11, "jd58063cd");

    spu_begin_image(12);
    tj_jfe031af6spu_recomp_register();
    spu_workload_register_img(0xFE031AF67ED048DEULL, tj_jfe031af6spu_func_00000000, 12, "jfe031af6");

    spu_begin_image(13);
    tj_j78fea84espu_recomp_register();
    spu_workload_register_img(0x78FEA84E1C23BB5CULL, tj_j78fea84espu_func_00000000, 13, "j78fea84e");

    spu_begin_image(14);
    tj_jd8945a0bspu_recomp_register();
    spu_workload_register_img(0xD8945A0BA2D3D054ULL, tj_jd8945a0bspu_func_00000000, 14, "jd8945a0b");

    spu_begin_image(15);
    tj_jdd6f7bfdspu_recomp_register();
    spu_workload_register_img(0xDD6F7BFDDFCF7894ULL, tj_jdd6f7bfdspu_func_00000000, 15, "jdd6f7bfd");

}

