/* mcgpu_prog.cuh — выгрузка McgProg на устройство (общая для биомов и рельефа). */
#pragma once
#include "mcgpu_internal.h"
#include "mcgpu_dev.cuh"

struct GProg {
    DProg d;
    McgNode *nodes = nullptr; McgNoise *noise = nullptr; McgLayer *layers = nullptr; McgOct *octs = nullptr;
    McgSpline *sp = nullptr; float *splf = nullptr; int *spc = nullptr; int *arr = nullptr; double *thr = nullptr;
    void release() {
        cudaFree(nodes); cudaFree(noise); cudaFree(layers); cudaFree(octs); cudaFree(sp); cudaFree(splf); cudaFree(spc); cudaFree(arr); cudaFree(thr);
        nodes = nullptr; noise = nullptr; layers = nullptr; octs = nullptr; sp = nullptr; splf = nullptr; spc = nullptr; arr = nullptr; thr = nullptr;
    }
    bool upload(const McgProg *p) {
        bool ok = mcgpu_upload(&nodes, p->nodes, (size_t)p->nnodes) && mcgpu_upload(&noise, p->noise, (size_t)p->nnoise) &&
                  mcgpu_upload(&layers, p->layers, (size_t)p->nlayers) && mcgpu_upload(&octs, p->octs, (size_t)p->noct) &&
                  mcgpu_upload(&sp, p->sp, (size_t)p->nsp) && mcgpu_upload(&splf, p->splf, (size_t)p->nsplf) &&
                  mcgpu_upload(&spc, p->spc, (size_t)p->nspc) && mcgpu_upload(&arr, p->arr, (size_t)p->narr) &&
                  mcgpu_upload(&thr, p->thr, (size_t)p->nthr);
        if (!ok) { release(); return false; }
        d.nodes = nodes; d.noise = noise; d.layers = layers; d.octs = octs; d.sp = sp; d.splf = splf; d.spc = spc; d.arr = arr; d.thr = thr;
        d.nnodes = p->nnodes;
        for (int i = 0; i < MCG_MAX_ROOTS; i++) d.root[i] = i < p->nroots ? p->root[i] : -1;
        return true;
    }
};
