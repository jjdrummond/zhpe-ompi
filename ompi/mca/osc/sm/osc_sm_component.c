/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012      Sandia National Laboratories.  All rights reserved.
 * Copyright (c) 2014-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2017-2018 Hewlett Packard Enterprise Development LP.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"

#include "ompi/mca/osc/osc.h"
#include "ompi/mca/osc/base/base.h"
#include "ompi/mca/osc/base/osc_base_obj_convert.h"
#include "ompi/request/request.h"
#include "opal/util/sys_limits.h"
#include "opal/include/opal/align.h"

#include "osc_sm.h"
#include <pthread.h>
#include <unistd.h>
#include <libpmem.h>

static int component_open(void);
static int component_init(bool enable_progress_threads, bool enable_mpi_threads);
static int component_finalize(void);
static int component_query(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                           struct ompi_communicator_t *comm, struct ompi_info_t *info,
                           int flavor);
static int component_select(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                            struct ompi_communicator_t *comm, struct ompi_info_t *info,
                            int flavor, int *model);

/* Can't pass parameters to opal_progress_register, so we define module as a global variable. */
#define FSM_ATOMIC_SERVER_PROGRESS

#ifdef FSM_ATOMIC_SERVER_PROGRESS
ompi_osc_sm_module_t *module = NULL;
#endif

ompi_osc_sm_component_t mca_osc_sm_component = {
    { /* ompi_osc_base_component_t */
        .osc_version = {
            OMPI_OSC_BASE_VERSION_3_0_0,
            .mca_component_name = "sm",
            MCA_BASE_MAKE_VERSION(component, OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION,
                                  OMPI_RELEASE_VERSION),
            .mca_open_component = component_open,
        },
        .osc_data = { /* mca_base_component_data */
            /* The component is not checkpoint ready */
            MCA_BASE_METADATA_PARAM_NONE
        },
        .osc_init = component_init,
        .osc_query = component_query,
        .osc_select = component_select,
        .osc_finalize = component_finalize,
    }
};


ompi_osc_sm_module_t ompi_osc_sm_module_template = {
    {
        .osc_win_shared_query = ompi_osc_sm_shared_query,

        .osc_win_attach = ompi_osc_sm_attach,
        .osc_win_detach = ompi_osc_sm_detach,
        .osc_free = ompi_osc_sm_free,

        .osc_put = ompi_osc_sm_put,
        .osc_get = ompi_osc_sm_get,
        .osc_accumulate = ompi_osc_fsm_accumulate,
        .osc_compare_and_swap = ompi_osc_sm_compare_and_swap,
        .osc_fetch_and_op = ompi_osc_sm_fetch_and_op,
        .osc_get_accumulate = ompi_osc_sm_get_accumulate,

        .osc_rput = ompi_osc_sm_rput,
        .osc_rget = ompi_osc_sm_rget,
        .osc_raccumulate = ompi_osc_sm_raccumulate,
        .osc_rget_accumulate = ompi_osc_sm_rget_accumulate,

        .osc_fence = ompi_osc_fsm_fence,

        .osc_start = ompi_osc_sm_start,
        .osc_complete = ompi_osc_sm_complete,
        .osc_post = ompi_osc_sm_post,
        .osc_wait = ompi_osc_sm_wait,
        .osc_test = ompi_osc_sm_test,

        .osc_lock = ompi_osc_sm_lock,
        .osc_unlock = ompi_osc_sm_unlock,
        .osc_lock_all = ompi_osc_sm_lock_all,
        .osc_unlock_all = ompi_osc_sm_unlock_all,

        .osc_sync = ompi_osc_sm_sync,
        .osc_flush = ompi_osc_sm_flush,
        .osc_flush_all = ompi_osc_sm_flush_all,
        .osc_flush_local = ompi_osc_sm_flush_local,
        .osc_flush_local_all = ompi_osc_sm_flush_local_all,

        .osc_set_info = ompi_osc_sm_set_info,
        .osc_get_info = ompi_osc_sm_get_info
    }
};



static inline void
fsm_set32(ompi_osc_sm_module_t *module,
         int target,
         size_t offset,
         uint32_t val)
{
    int32_t *addr = (int32_t*) ((char*) &module->node_states[target].lock + offset);
    (*addr = val);

    smp_wmb();

    /* fsm_atomic_exec called pmem_invalidate just before fsm_set32 was called. */
    /* Call pmem_persist so the set value is visible on other nodes. */
    pmem_persist(addr, sizeof(int32_t));
}

static inline void
fsm_add32(ompi_osc_sm_module_t *module,
         int target,
         size_t offset,
         uint32_t delta)
{
    int32_t *addr = (int32_t*) ((char*) &module->node_states[target].lock + offset);
    (*addr += delta);

    smp_wmb();

    /* fsm_atomic_exec called pmem_invalidate just before fsm_add32 was called. */
    /* Call pmem_persist so the modified value is visible on other nodes. */
    pmem_persist(addr, sizeof(int32_t));
}


static inline uint32_t
fsm_fetch32(ompi_osc_sm_module_t *module,
           int target,
           size_t offset)
{
    int32_t *addr = (int32_t*) ((char*) &module->node_states[target].lock + offset);
    return *addr;
}


static int
component_open(void)
{
    return OMPI_SUCCESS;
}


static int
component_init(bool enable_progress_threads, bool enable_mpi_threads)
{
    return OMPI_SUCCESS;
}


static int
component_finalize(void)
{
    /* clean up requests free list */
    return OMPI_SUCCESS;
}


static int
check_win_ok(ompi_communicator_t *comm, int flavor)
{
    if (! (MPI_WIN_FLAVOR_SHARED == flavor
           || MPI_WIN_FLAVOR_ALLOCATE == flavor) ) {
        return OMPI_ERR_NOT_SUPPORTED;
    }

   /* remote peers can use fabric-shared memory */
   /* ---
    if (ompi_group_have_remote_peers (comm->c_local_group)) {
        return OMPI_ERR_RMA_SHARED;
    }
    --- */

    return OMPI_SUCCESS;
}


static int
component_query(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                struct ompi_communicator_t *comm, struct ompi_info_t *info,
                int flavor)
{
    int ret;
    if (OMPI_SUCCESS != (ret = check_win_ok(comm, flavor))) {
        if (OMPI_ERR_NOT_SUPPORTED == ret) {
            return -1;
        }
        return ret;
    }

    return 100;
}

/* Actually update the protected variable, then persist it. */
static inline void
fsm_atomic_op(ompi_osc_sm_module_t* module, ompi_osc_fsm_atomic_t* fsm_ac_p, uint32_t* target)
{
    switch (fsm_ac_p->op) {
        case add:
            fsm_ac_p->result = opal_atomic_add_32((int32_t*)target, fsm_ac_p->param);
            pmem_persist(target, sizeof(*target));
            break;
        default:
            break;
    }
}

static inline void
fsm_atomic_exec(ompi_osc_sm_module_t* module, int src_rank, size_t fsm_offset, size_t offset)
{
    ompi_osc_fsm_atomic_t* fsm_ac_p;
    fsm_ac_p = (ompi_osc_fsm_atomic_t*) ((char*) &module->node_states[src_rank].lock + fsm_offset);

    /* Invalidate appropriate cachelines so we get fresh data */
    /* before fsm_add32 was called. */
    pmem_invalidate(fsm_ac_p, sizeof(struct ompi_osc_fsm_atomic_t));

    if (fsm_ac_p->ack < fsm_ac_p->request) {
        //one rank can only update itself, that means rank has to be equal to target rank
        int rank = ompi_comm_rank(module->comm);
    int target_rank = fsm_ac_p->target;
        if (rank != target_rank) {
        return;
        }
        uint32_t* target = (uint32_t *)((char*) &module->node_states[target_rank].lock + offset);
        fsm_atomic_op(module, fsm_ac_p, target);
        fsm_ac_p->ack = fsm_ac_p->request;
        /* flush cache containing the ack so that requester can see it. */
        pmem_persist(fsm_ac_p, sizeof(struct ompi_osc_fsm_atomic_t));
    }
}


static int ompi_fsm_atomic_server_progress(void)
{
   int rank;
    int comm_size = ompi_comm_size(module->comm);

    for (rank = 0; rank < comm_size; rank++) {

        fsm_atomic_exec(module, rank, offsetof(ompi_osc_sm_lock_t, counter_ac), offsetof(ompi_osc_sm_lock_t, counter));
        fsm_atomic_exec(module, rank, offsetof(ompi_osc_sm_lock_t, counter2_ac), offsetof(ompi_osc_sm_lock_t, counter2));
        fsm_atomic_exec(module, rank, offsetof(ompi_osc_sm_lock_t, write_ac), offsetof(ompi_osc_sm_lock_t, write));
        fsm_atomic_exec(module, rank, offsetof(ompi_osc_sm_lock_t, read_ac), offsetof(ompi_osc_sm_lock_t, read));
        fsm_atomic_exec(module, rank, offsetof(ompi_osc_sm_lock_t, accumulate_ac), offsetof(ompi_osc_sm_lock_t, accumulate));
    }

    return 0;
}


/* Note: we must perfectly cache-align all data structures stored on FAM */
/*       and also protect those cache lines from false sharing. */

static int
component_select(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                 struct ompi_communicator_t *comm, struct ompi_info_t *info,
                 int flavor, int *model)
{
    #ifndef FSM_ATOMIC_SERVER_PROGRESS
    ompi_osc_sm_module_t *module = NULL;
    #endif

    size += OPAL_ALIGN_PAD_AMOUNT(size, CACHELINE_SZ);
    int my_rank = ompi_comm_rank (comm);


    int comm_size = ompi_comm_size (comm);
    int ret = OMPI_ERROR;

    if (OMPI_SUCCESS != (ret = check_win_ok(comm, flavor))) {
        return ret;
    }

    /* create module structure */
    module = (ompi_osc_sm_module_t*)
        calloc(1, sizeof(ompi_osc_sm_module_t));
    if (NULL == module) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;

    OBJ_CONSTRUCT(&module->lock, opal_mutex_t);

    /* fill in the function pointer part */
    memcpy(module, &ompi_osc_sm_module_template,
           sizeof(ompi_osc_base_module_t));

    /* need our communicator for collectives in next phase */
    ret = ompi_comm_dup(comm, &module->comm);
    if (OMPI_SUCCESS != ret) goto error;

    module->flavor = flavor;

    /* create the segment */
    if (1 == comm_size) {
        module->segment_base = NULL;
        module->sizes = malloc(sizeof(size_t));
        if (NULL == module->sizes) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        module->bases = malloc(sizeof(void*));
        if (NULL == module->bases) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;

        module->sizes[0] = size;
        module->bases[0] = malloc(size);
        if (NULL == module->bases[0]) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;

        module->global_state = malloc(sizeof(ompi_osc_sm_global_state_t));
        if (NULL == module->global_state) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        module->node_states = malloc(sizeof(ompi_osc_sm_node_state_t));
        if (NULL == module->node_states) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        module->posts = calloc (1, sizeof(module->posts[0]) + sizeof (uint64_t));
        if (NULL == module->posts) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        module->posts[0] = (uint64_t *) (module->posts + 1);
    } else {
        unsigned long total, *rbuf;
        int i, flag;
        size_t pagesize;
        size_t state_size;
        size_t segment_size;
        size_t posts_size, post_size = (comm_size + 63) / 64;

        OPAL_OUTPUT_VERBOSE((1, ompi_osc_base_framework.framework_output,
                             "allocating shared memory region of size %ld\n", (long) size));

        /* get the pagesize */
        pagesize = opal_getpagesize();

        rbuf = malloc(sizeof(unsigned long) * comm_size);
        if (NULL == rbuf) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;

        module->noncontig = false;
        if (OMPI_SUCCESS != ompi_info_get_bool(info, "alloc_shared_noncontig",
                                               &module->noncontig, &flag)) {
            goto error;
        }

        if (module->noncontig) {
            total = ((size - 1) / pagesize + 1) * pagesize;
        } else {
            total = size;
        }
        ret = module->comm->c_coll->coll_allgather(&total, 1, MPI_UNSIGNED_LONG,
                                                  rbuf, 1, MPI_UNSIGNED_LONG,
                                                  module->comm,
                                                  module->comm->c_coll->coll_allgather_module);
        if (OMPI_SUCCESS != ret) return ret;

        total = 0;
        for (i = 0 ; i < comm_size ; ++i) {
            rbuf[i] += OPAL_ALIGN_PAD_AMOUNT(rbuf[i], opal_getpagesize());
            total += rbuf[i];
        }

    /* user opal/shmem directly to create a shared memory segment */
    state_size = sizeof(ompi_osc_sm_global_state_t);
        state_size += sizeof(ompi_osc_sm_node_state_t) * comm_size;
        state_size += OPAL_ALIGN_PAD_AMOUNT(state_size, CACHELINE_SZ);

        posts_size = comm_size * post_size * sizeof (uint64_t);
        posts_size += OPAL_ALIGN_PAD_AMOUNT(posts_size, CACHELINE_SZ);

        segment_size = total + pagesize + state_size + posts_size + CACHELINE_SZ;
        if (0 == ompi_comm_rank (module->comm)) {

            char *data_file;
            if (asprintf(&data_file, "%s"OPAL_PATH_SEP"shared_window_%d.%s",
                         ompi_process_info.proc_session_dir,
                         ompi_comm_get_cid(module->comm),
                         ompi_process_info.nodename) < 0) {
                return OMPI_ERR_OUT_OF_RESOURCE;
            }

            ret = opal_shmem_segment_create (&module->seg_ds, data_file, segment_size);
            free(data_file);
            if (OPAL_SUCCESS != ret) {
                goto error;
            }
            module->segment_base = opal_shmem_segment_attach (&module->seg_ds);
            if (NULL == module->segment_base) {
                goto error;
            }
            memset(module->segment_base, 0, segment_size);
            pmem_persist(module->segment_base,segment_size);

        }

    ret = module->comm->c_coll->coll_bcast (&module->seg_ds, sizeof (module->seg_ds), MPI_BYTE, 0,
                           module->comm, module->comm->c_coll->coll_bcast_module);
    if (OMPI_SUCCESS != ret) {
        goto error;
    }

        if (0 != ompi_comm_rank (module->comm)) {
            module->segment_base = opal_shmem_segment_attach (&module->seg_ds);
            if (NULL == module->segment_base) {
                goto error;
            }
        }

        module->sizes = malloc(sizeof(size_t) * comm_size);
        if (NULL == module->sizes) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        module->bases = malloc(sizeof(void*) * comm_size);
        if (NULL == module->bases) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        module->posts = calloc (comm_size, sizeof (module->posts[0]));
        if (NULL == module->posts) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;


        /* set module->posts[0] first to ensure cacheline alignment */
        module->posts[0] = OPAL_ALIGN_PTR((uintptr_t) module->segment_base, CACHELINE_SZ, void *);
        module->global_state = (ompi_osc_sm_global_state_t *) ((char *) module->posts[0] + posts_size);
        module->node_states = (ompi_osc_sm_node_state_t *) (module->global_state + 1);

        for (i = 0, total = state_size + posts_size ; i < comm_size ; ++i) {
            if (i > 0) {
                module->posts[i] = module->posts[i - 1] + post_size;
            }

            module->sizes[i] = rbuf[i];
            if (module->sizes[i]) {
                module->bases[i] = ((char *) module->posts[0]) + total;
                total += rbuf[i];
            } else {
                module->bases[i] = NULL;
            }
        }

        free(rbuf);
    }

    /* initialize my state shared */
    module->my_node_state = &module->node_states[ompi_comm_rank(module->comm)];

    *base = module->bases[ompi_comm_rank(module->comm)];
    pmem_invalidate(module->bases[my_rank], module->sizes[my_rank]);

    opal_atomic_init(&module->my_node_state->accumulate_lock, OPAL_ATOMIC_UNLOCKED);

    /* share everyone's displacement units. */
    module->disp_units = malloc(sizeof(int) * comm_size);
    ret = module->comm->c_coll->coll_allgather(&disp_unit, 1, MPI_INT,
                                              module->disp_units, 1, MPI_INT,
                                              module->comm,
                                              module->comm->c_coll->coll_allgather_module);
    if (OMPI_SUCCESS != ret) goto error;

    module->start_group = NULL;
    module->post_group = NULL;

    /* initialize synchronization code */
    module->my_sense = 1;

    module->outstanding_locks = calloc(comm_size, sizeof(enum ompi_osc_sm_locktype_t));
    if (NULL == module->outstanding_locks) {
        ret = OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        goto error;
    }

    if (0 == ompi_comm_rank(module->comm)) {
#if HAVE_PTHREAD_CONDATTR_SETPSHARED && HAVE_PTHREAD_MUTEXATTR_SETPSHARED
        pthread_mutexattr_t mattr;
        pthread_condattr_t cattr;
        bool blocking_fence=false;
        int flag;

        if (OMPI_SUCCESS != ompi_info_get_bool(info, "blocking_fence",
                                               &blocking_fence, &flag)) {
            goto error;
        }

        if (flag && blocking_fence) {
            ret = pthread_mutexattr_init(&mattr);
            ret = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
            if (ret != 0) {
                module->global_state->use_barrier_for_fence = 1;
            } else {
                ret = pthread_mutex_init(&module->global_state->mtx, &mattr);
                if (ret != 0) {
                    module->global_state->use_barrier_for_fence = 1;
                } else {
                    pthread_condattr_init(&cattr);
                    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
                    ret = pthread_cond_init(&module->global_state->cond, &cattr);
                    if (ret != 0) return OMPI_ERROR;
                    pthread_condattr_destroy(&cattr);
                }
            }
            module->global_state->use_barrier_for_fence = 0;
            module->global_state->sense = module->my_sense;
            module->global_state->count = comm_size;
            pthread_mutexattr_destroy(&mattr);
        } else {
            module->global_state->use_barrier_for_fence = 1;
        }
#else
        module->global_state->use_barrier_for_fence = 1;
#endif
    }

    /* Every rank should flush their node_state */
    pmem_persist(&module->node_states[my_rank], module->sizes[my_rank]);
    ret = module->comm->c_coll->coll_barrier(module->comm,
                                            module->comm->c_coll->coll_barrier_module);
    if (OMPI_SUCCESS != ret) goto error;

    *model = MPI_WIN_UNIFIED;

    win->w_osc_module = &module->super;

    opal_progress_register(ompi_fsm_atomic_server_progress);
    return OMPI_SUCCESS;

 error:
    win->w_osc_module = &module->super;
    ompi_osc_sm_free (win);

    return ret;
}


int
ompi_osc_sm_shared_query(struct ompi_win_t *win, int rank, size_t *size, int *disp_unit, void *baseptr)
{
    ompi_osc_sm_module_t *module =
        (ompi_osc_sm_module_t*) win->w_osc_module;

    if (module->flavor != MPI_WIN_FLAVOR_SHARED) {
        return MPI_ERR_WIN;
    }

    if (MPI_PROC_NULL != rank) {
        *size = module->sizes[rank];
        *((void**) baseptr) = module->bases[rank];
        *disp_unit = module->disp_units[rank];
    } else {
        int i = 0;

        *size = 0;
        *((void**) baseptr) = NULL;
        *disp_unit = 0;
        for (i = 0 ; i < ompi_comm_size(module->comm) ; ++i) {
            if (0 != module->sizes[i]) {
                *size = module->sizes[i];
                *((void**) baseptr) = module->bases[i];
                *disp_unit = module->disp_units[i];
                break;
            }
        }
    }

    return OMPI_SUCCESS;
}


int
ompi_osc_sm_attach(struct ompi_win_t *win, void *base, size_t len)
{
    ompi_osc_sm_module_t *module =
        (ompi_osc_sm_module_t*) win->w_osc_module;

    if (module->flavor != MPI_WIN_FLAVOR_DYNAMIC) {
        return MPI_ERR_RMA_ATTACH;
    }
    return OMPI_SUCCESS;
}


int
ompi_osc_sm_detach(struct ompi_win_t *win, const void *base)
{
    ompi_osc_sm_module_t *module =
        (ompi_osc_sm_module_t*) win->w_osc_module;

    if (module->flavor != MPI_WIN_FLAVOR_DYNAMIC) {
        return MPI_ERR_RMA_ATTACH;
    }
    return OMPI_SUCCESS;
}


int
ompi_osc_sm_free(struct ompi_win_t *win)
{
    ompi_osc_sm_module_t *module =
        (ompi_osc_sm_module_t*) win->w_osc_module;
        /* clean up requests free list */

    int my_rank = ompi_comm_rank(module->comm);

    /* free memory */
    if (NULL != module->segment_base) {
        /* synchronize */
        module->comm->c_coll->coll_barrier(module->comm,
                                          module->comm->c_coll->coll_barrier_module);

        opal_progress_unregister(ompi_fsm_atomic_server_progress);

        if (0 == ompi_comm_rank (module->comm)) {
            opal_shmem_unlink (&module->seg_ds);
        }

        opal_shmem_segment_detach (&module->seg_ds);
    } else {
        free(module->node_states);
        free(module->global_state);
        free(module->bases[0]);
    }
    free(module->disp_units);
    free(module->outstanding_locks);
    free(module->sizes);
    free(module->bases);

    free (module->posts);

    /* cleanup */
    ompi_comm_free(&module->comm);

    OBJ_DESTRUCT(&module->lock);


    free(module);

    return OMPI_SUCCESS;
}


int
ompi_osc_sm_set_info(struct ompi_win_t *win, struct ompi_info_t *info)
{
    ompi_osc_sm_module_t *module =
        (ompi_osc_sm_module_t*) win->w_osc_module;

    /* enforce collectiveness... */
    return module->comm->c_coll->coll_barrier(module->comm,
                                             module->comm->c_coll->coll_barrier_module);
}


int
ompi_osc_sm_get_info(struct ompi_win_t *win, struct ompi_info_t **info_used)
{
    ompi_osc_sm_module_t *module =
        (ompi_osc_sm_module_t*) win->w_osc_module;

    ompi_info_t *info = OBJ_NEW(ompi_info_t);
    if (NULL == info) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;

    if (module->flavor == MPI_WIN_FLAVOR_SHARED) {
        ompi_info_set(info, "blocking_fence",
                      (1 == module->global_state->use_barrier_for_fence) ? "true" : "false");
        ompi_info_set(info, "alloc_shared_noncontig",
                      (module->noncontig) ? "true" : "false");
    }

    *info_used = info;

    return OMPI_SUCCESS;
}
