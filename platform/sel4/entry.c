/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */
#include <autoconf.h>
#include <sel4/sel4.h>
#include <sel4/helpers.h>
#include <stdlib.h>
#include <sel4utils/util.h>
#include <allocman/vka.h>
#include <allocman/bootstrap.h>
#include <sel4platsupport/timer.h>
#include <simple/simple_helpers.h>
#include <sel4platsupport/io.h>
#include <sel4utils/iommu_dma.h>
#include <sel4utils/page_dma.h>
#include <sel4utils/time_server/client.h>
#include <sel4/benchmark_utilisation_types.h>
#include <allocman/utspace/utspace.h>
#include <sel4platsupport/arch/io.h>
#include <platsupport/timer.h>
#include <bmk-core/core.h>
#include <bmk-core/pgalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/string.h>
#include <bmk-core/sched.h>
#include <bmk-core/mainthread.h>
#include <sel4platsupport/platsupport.h>
#include <bmk-core/types.h>
#include <sel4/kernel.h>
#include <sel4utils/stack.h>
#include <rumprun-base/rumprun.h>
/* global static memory for init */
static sel4utils_alloc_data_t alloc_data;

/* dimensions of virtual memory for the allocator to use */
#define ALLOCATOR_VIRTUAL_POOL_SIZE ((PAGE_SIZE_4K) * 4000)

/* allocator static pool */
#define ALLOCATOR_STATIC_POOL_SIZE ((PAGE_SIZE_4K) * 20)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

/* Damn linker errors: This symbol is overrided by files_to_obj.sh it is included
    here to prevent 'undefined reference to `_cpio_archive`' linker errors */
char _cpio_archive[1];

/* Environment global data */
struct env env = {
    .spldepth = 0,
    .mask_the_mask = 0,
    .should_wakeup = 0
};

extern vspace_t *muslc_this_vspace;
extern reservation_t muslc_brk_reservation;
extern void *muslc_brk_reservation_start;
sel4utils_res_t muslc_brk_reservation_memory;

static void
init_allocator(env_t env)
{
    UNUSED int error;
    UNUSED reservation_t virtual_reservation;

    /* initialise allocator */
    allocman_t *allocator = bootstrap_use_current_1level(simple_get_cnode(&env->simple),
                                                         simple_get_cnode_size_bits(&env->simple),
                                                         simple_last_valid_cap(&env->simple) + 1,
                                                         BIT(simple_get_cnode_size_bits(&env->simple)),
                                                         ALLOCATOR_STATIC_POOL_SIZE,
                                                         allocator_mem_pool);
    ZF_LOGF_IF(allocator == NULL, "Failed to bootstrap allocator");
    allocman_make_vka(&env->vka, allocator);

    int num_regions = custom_get_num_regions(&env->custom_simple);
    pmem_region_t *regions = allocman_mspace_alloc(allocator, sizeof(pmem_region_t) * num_regions, &error);
    ZF_LOGF_IF(error, "allocman_mspace_alloc failed to allocate regions");
    error = custom_get_region_list(&env->custom_simple, num_regions, regions);
    ZF_LOGF_IF(num_regions != error, "calloc returned NULL");
    allocman_add_simple_untypeds_with_regions(allocator, &env->simple, num_regions, regions);
    allocman_mspace_free(allocator, regions, sizeof(pmem_region_t) * num_regions);

    error = custom_simple_vspace_bootstrap_frames(&env->custom_simple, &env->vspace, &alloc_data, &env->vka);

    error = sel4utils_reserve_range_no_alloc(&env->vspace, &muslc_brk_reservation_memory, 1048576, seL4_AllRights, 1, &muslc_brk_reservation_start);
    ZF_LOGF_IF(error, "Failed to reserve_range");
    muslc_this_vspace = &env->vspace;
    muslc_brk_reservation.res = &muslc_brk_reservation_memory;


    /* switch the allocator to a virtual memory pool */
    void *vaddr;
    virtual_reservation = vspace_reserve_range(&env->vspace, ALLOCATOR_VIRTUAL_POOL_SIZE,
                                               seL4_AllRights, 1, &vaddr);
    ZF_LOGF_IF(virtual_reservation.res == 0, "Failed to switch allocator to virtual memory pool");

    bootstrap_configure_virtual_pool(allocator, vaddr, ALLOCATOR_VIRTUAL_POOL_SIZE,
                                     simple_get_pd(&env->simple));

}

static void
provide_vmem(env_t env)
{
    void *osend;

    bmk_core_init(BMK_THREAD_STACK_PAGE_ORDER);

    vspace_new_pages_config_t config;
    size_t rumprun_size = env->custom_simple.rumprun_memory_size;
    int page_size_bits = sel4_page_size_bits_for_memory_region(rumprun_size);
    env->rump_mapping_page_size_bits = page_size_bits;
    env->rump_mapping_page_type = kobject_get_type(KOBJECT_FRAME, page_size_bits);
    ZF_LOGW_IF(rumprun_size % BIT(page_size_bits) != 0, "Warning: Memory size is being truncated by: 0x%zx", rumprun_size % BIT(page_size_bits));
    size_t rumprun_pages = rumprun_size / BIT(page_size_bits);
    printf("num pages %zd with size: %d bits\n", rumprun_pages, page_size_bits);
    if (default_vspace_new_pages_config(rumprun_pages, page_size_bits, &config)) {
        ZF_LOGF("Failed to create config");
    }
    if (vspace_new_pages_config_use_device_ut(true, &config)) {
        ZF_LOGF("Failed to set device_ram");
    }

    osend = vspace_new_pages_with_config(&env->vspace, &config, seL4_CapRights_new(1, 1, 1));
    ZF_LOGF_IF(osend == NULL, "vspace returned null");

    printf("Starting paddr: %p\n", osend);
    bmk_pgalloc_loadmem((uintptr_t) osend, (uintptr_t) osend + rumprun_size);

    bmk_memsize = rumprun_size;
}

void rump_irq_handle(int intr)
{
    sync_bin_sem_wait(&env.spl_semaphore);

    ZF_LOGF_IF(env.spldepth != 0, "spldepth should be 0.  This thread should be blocked.");
    if (env.should_wakeup != 0) {
        seL4_Signal(env.custom_simple.timer_config.timer_ntfn);
    }

    env.mask_the_mask = 1;

    isr(intr);
    sync_bin_sem_post(&env.spl_semaphore);
    env.mask_the_mask = 0;

}

static void wait_for_pci_interrupt(void * UNUSED _a, void * UNUSED _b, void * UNUSED _c)
{

    /* Set up TLS for main thread.  The main thread can't do this itself
        so this thread is used before it actually handles PCI stuff */
    seL4_UserContext context;
    int res = seL4_TCB_ReadRegisters(simple_get_tcb(&env.simple), 1, 0, (sizeof(seL4_UserContext) / sizeof(seL4_Word)), &context );
    ZF_LOGF_IF(res, "Could not read registers");
    context.tls_base = (seL4_Word)&env.tls_base_ptr;
    /* When you call write registers on a thread, seL4 changes it to be restarted.
       Because the target thread is of a lower priority, its last instruction was an
       invocation that set the current thread to running. The eip needs to be incremented
       so that the target thread is resumed correctly instead of recalling the invocation. */
    seL4_Word pc = sel4utils_get_instruction_pointer(context);
    sel4utils_set_instruction_pointer(&context, pc + ARCH_SYSCALL_INSTRUCTION_SIZE);
    res = seL4_TCB_WriteRegisters(simple_get_tcb(&env.simple), 1, 0, (sizeof(seL4_UserContext) / sizeof(seL4_Word)), &context );
    ZF_LOGF_IF(res, "Could not write registers");

    env.mask_the_mask = 0;
    while (1) {
        seL4_Word sender_badge;
        seL4_Wait(env.pci_notification.cptr, &sender_badge);
        rump_irq_handle(sender_badge);
    }
}

int init_rumprun(custom_simple_t *custom_simple)
{
    if (custom_simple != &env.custom_simple) {
        env.custom_simple = *custom_simple;
    }
    if (&env.simple != env.custom_simple.simple) {
        env.simple = *env.custom_simple.simple;
    }

    /* initialse cspace, vspace and untyped memory allocation */
    init_allocator(&env);


    int res = sel4platsupport_new_io_ops(env.vspace, env.vka, &env.io_ops);
    ZF_LOGF_IF(res != 0, "sel4platsupport_new_io_ops failed");

    res = sel4platsupport_get_io_port_ops(&env.io_ops.io_port_ops, &env.simple);
    ZF_LOGF_IF(res != 0, "sel4platsupport_get_io_port_ops failed");

    sel4platsupport_new_malloc_ops(&env.io_ops.malloc_ops);

    if (is_ltimer(custom_simple)) {
        sel4utils_rpc_ltimer_init(&custom_simple->timer_config.ltimer.ltimer, env.io_ops,
                                  custom_simple->rpc_ep, TIMER_LABEL);
    }

    cons_init();

    res = vka_alloc_notification(&env.vka, &env.pci_notification);
    ZF_LOGF_IF(res != 0, "Failed to allocate notification object");
    res = vka_alloc_notification(&env.vka, &env.spl_notification);
    ZF_LOGF_IF(res != 0, "Failed to allocate notification object");
    sync_bin_sem_init(&env.spl_semaphore, env.spl_notification.cptr, 1);

    sel4utils_thread_config_t thread_config = thread_config_default(&env.simple,
        simple_get_cnode(&env.simple), seL4_NilData, seL4_CapNull, custom_get_priority(&env.custom_simple));

    res = sel4utils_configure_thread_config(&env.vka, &env.vspace, &env.vspace, thread_config, &env.pci_thread);
    ZF_LOGF_IF(res != 0, "Configure thread failed");


    res = seL4_TCB_SetPriority(simple_get_tcb(&env.simple), custom_get_priority(&env.custom_simple) - 1);
    ZF_LOGF_IF(res != 0, "seL4_TCB_SetPriority thread failed");

    NAME_THREAD(env.pci_thread.tcb.cptr, "pci thread");
    res = sel4utils_start_thread(&env.pci_thread, wait_for_pci_interrupt, NULL, NULL,
                                 1);

    ZF_LOGF_IF(res != 0, "sel4utils_start_thread(wait_for_pci_interrupt) failed");

#ifdef CONFIG_IOMMU
    seL4_CPtr io_space = simple_init_cap(&env.simple, seL4_CapIOSpace);
    res = sel4utils_make_iommu_dma_alloc(&env.vka, &env.vspace, &env.io_ops.dma_manager, 1, &io_space);
    ZF_LOGF_IF(res != 0, "sel4utils_make_iommu_dma_alloc failed");
#else
    res = sel4utils_new_page_dma_alloc(&env.vka, &env.vspace, &env.io_ops.dma_manager);
    ZF_LOGF_IF(res != 0, "sel4utils_new_page_dma_alloc failed");
#endif

    bmk_printf("rump kernel bare metal bootstrap\n\n");
    x86_initclocks();

    bmk_sched_init();
    provide_vmem(&env);
    intr_init();
    struct rumprun_boot_config rumprun_config = {(char *)custom_get_cmdline(&env.custom_simple), CONFIG_RUMPRUN_TMPFS_NUM_MiB};

    bmk_sched_startmain(bmk_mainthread, (void *) &rumprun_config);

    return 0;
}