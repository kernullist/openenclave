// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "sgxload.h"
#if defined(OE_USE_LIBSGX)
#include <sgx_enclave_common.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "linux/sgxioctl.h"
#elif defined(_WIN32)
#include <Windows.h>
#endif

#include <assert.h>
#include <openenclave/bits/safecrt.h>
#include <openenclave/bits/safemath.h>
#include <openenclave/internal/aesm.h>
#include <openenclave/internal/raise.h>
#include <openenclave/internal/sgxcreate.h>
#include <openenclave/internal/sgxsign.h>
#include <openenclave/internal/sgxtypes.h>
#include <openenclave/internal/trace.h>
#include <openenclave/internal/utils.h>
#include "enclave.h"
#include "memalign.h"
#include "sgxmeasure.h"
#include "signkey.h"

static uint32_t _make_memory_protect_param(uint64_t inflags, bool simulate)
{
    uint32_t outflags = 0;

    if (inflags & SGX_SECINFO_TCS)
    {
        if (simulate)
        {
/* TCS can be read and written in simulation mode */
#if defined(__linux__)
            outflags = PROT_READ | PROT_WRITE;
#elif defined(_WIN32)
            outflags = PAGE_READWRITE;
#endif
        }
        else
        {
#if defined(OE_USE_LIBSGX)
            /* libsgx is only used when not in simulation mode */
            outflags = ENCLAVE_PAGE_THREAD_CONTROL | ENCLAVE_PAGE_READ |
                       ENCLAVE_PAGE_WRITE;
#elif defined(__linux__)
            outflags = PROT_NONE;
#elif defined(_WIN32)
            outflags = PAGE_ENCLAVE_THREAD_CONTROL | PAGE_READWRITE;
#endif
        }
    }
    else if (inflags & SGX_SECINFO_REG)
    {
#if defined(OE_USE_LIBSGX)
        if (!simulate)
        {
            /* libsgx is only used when not in simulation mode */
            if (inflags & SGX_SECINFO_R)
                outflags |= ENCLAVE_PAGE_READ;

            if (inflags & SGX_SECINFO_W)
                outflags |= ENCLAVE_PAGE_WRITE;

            if (inflags & SGX_SECINFO_X)
                outflags |= ENCLAVE_PAGE_EXECUTE;
        }
        else
        {
/* simulation mode falls back to OS memory protection settings */
#endif
#if defined(__linux__)
            if (inflags & SGX_SECINFO_R)
                outflags |= PROT_READ;

            if (inflags & SGX_SECINFO_W)
                outflags |= PROT_WRITE;

            if (inflags & SGX_SECINFO_X)
                outflags |= PROT_EXEC;
#elif defined(_WIN32)
        if ((inflags & SGX_SECINFO_X) && (inflags & SGX_SECINFO_R) &&
            (inflags & SGX_SECINFO_W))
        {
            outflags = PAGE_EXECUTE_READWRITE;
        }
        else if ((inflags & SGX_SECINFO_X) && (inflags & SGX_SECINFO_R))
            outflags = PAGE_EXECUTE_READ;
        else if ((inflags & SGX_SECINFO_X))
            outflags = PAGE_EXECUTE;
        else if ((inflags & SGX_SECINFO_R) && (inflags & SGX_SECINFO_W))
            outflags = PAGE_READWRITE;
        else if ((inflags & SGX_SECINFO_R))
            outflags = PAGE_READONLY;
        else
            outflags = PAGE_NOACCESS;
#endif
#if defined(OE_USE_LIBSGX)
        }
#endif
    }

    return outflags;
}

static sgx_secs_t* _new_secs(uint64_t base, size_t size, bool debug)
{
    sgx_secs_t* secs = NULL;

    if (!(secs = (sgx_secs_t*)oe_memalign(OE_PAGE_SIZE, sizeof(sgx_secs_t))))
        return NULL;

    memset(secs, 0, sizeof(sgx_secs_t));
    secs->size = size;
    secs->base = base;

    secs->flags = SGX_FLAGS_MODE64BIT;
    if (debug)
        secs->flags |= SGX_FLAGS_DEBUG;

    /* what the driver sees with SGX SDK */
    secs->xfrm = SGX_ATTRIBUTES_DEFAULT_XFRM;

    /* COMMENT1: ssaframesize hardcoded to one for now */
    secs->ssaframesize = 1;

    /* secs->flags |= SGX_FLAGS_EINITTOKEN_KEY; */
    /* secs->flags |= SGX_FLAGS_PROVISION_KEY; */

    return secs;
}

/*
** Allocate memory for an enclave so that it has the following layout:
**
**    [............xxxxxxxxxxxxxxxxxxxxxxxx...............]
**     ^           ^                       ^              ^
**    MPTR        BASE                 BASE+SIZE      MPTR+SIZE*2
**
**    [MPTR...BASE]                 - unused
**    [BASE...BASE+SIZE]            - used
**    [BASE+SIZE...MPTR+SIZE*2]     - unused
*/
static void* _allocate_enclave_memory(size_t enclave_size, int fd)
{
#if defined(__linux__)

    /* Allocate enclave memory for simulated and real mode */
    void* result = NULL;
    void* base = NULL;
    void* mptr = MAP_FAILED;

    /* Map memory region */
    {
        int mprot = PROT_READ | PROT_WRITE | PROT_EXEC;
        int mflags = MAP_SHARED;
        uint64_t mmap_size = enclave_size;

        /* If no file descriptor, then perform anonymous mapping and double
         * the allocation size, so that BASE can be aligned on the SIZE
         * boundary. This isn't neccessary on hardware backed enclaves, since
         * the driver will do the alignment. */
        if (fd == -1)
        {
            mflags |= MAP_ANONYMOUS;
            if (oe_safe_mul_u64(mmap_size, 2, &mmap_size) != OE_OK)
                goto done;
        }

        mptr = mmap(NULL, mmap_size, mprot, mflags, fd, 0);

        if (mptr == MAP_FAILED)
            goto done;

        /* Exit early in hardware backed enclaves, since it's aligned. */
        if (fd > -1)
        {
            assert((uintptr_t)mptr % mmap_size == 0);
            result = mptr;
            goto done;
        }
    }

    /* Align BASE on a boundary of SIZE */
    {
        uint64_t n = enclave_size;
        uint64_t addr = ((uint64_t)mptr + (n - 1)) / n * n;
        base = (void*)addr;
    }

    /* Unmap [MPTR...BASE] */
    {
        uint8_t* start = (uint8_t*)mptr;
        uint8_t* end = (uint8_t*)base;

        if (start != end && munmap(start, end - start) != 0)
            goto done;
    }

    /* Unmap [BASE+SIZE...MPTR+SIZE*2] */
    {
        uint8_t* start = (uint8_t*)base + enclave_size;
        uint8_t* end = (uint8_t*)mptr + enclave_size * 2;

        if (start != end && munmap(start, end - start) != 0)
            goto done;
    }

    result = base;

done:

    /* On failure, unmap initially allocated region.
     * Linux will handle already unmapped regions within this original range */
    if (!result && mptr != MAP_FAILED)
        munmap(mptr, enclave_size * 2);

    return result;

#elif defined(_WIN32)

    /* Allocate enclave memory for simulated mode only */

    void* result = NULL;
    void* base = NULL;
    void* mptr = NULL;

    /* Allocate virtual memory for this enclave */
    if (!(mptr = VirtualAlloc(
              NULL,
              enclave_size * 2,
              MEM_COMMIT | MEM_RESERVE,
              PAGE_EXECUTE_READWRITE)))
    {
        goto done;
    }

    /* Align BASE on a boundary of SIZE */
    {
        uint64_t n = enclave_size;
        uint64_t addr = ((uint64_t)mptr + (n - 1)) / n * n;
        base = (void*)addr;
    }

    /* Release [MPTR...BASE] */
    {
        uint8_t* start = (uint8_t*)mptr;
        uint8_t* end = (uint8_t*)base;

        if (start != end && !VirtualFree(start, end - start, MEM_DECOMMIT))
            goto done;
    }

    /* Release [BASE+SIZE...MPTR+SIZE*2] */
    {
        uint8_t* start = (uint8_t*)base + enclave_size;
        uint8_t* end = (uint8_t*)mptr + enclave_size * 2;

        if (start != end && !VirtualFree(start, end - start, MEM_DECOMMIT))
            goto done;
    }

    result = base;

done:

    /* On failure, release the initial allocation. */
    if (!result && mptr)
        VirtualFree(mptr, 0, MEM_RELEASE);

    return result;

#endif /* defined(_WIN32) */
}

static oe_result_t _get_sig_struct(
    const oe_sgx_enclave_properties_t* properties,
    const OE_SHA256* mrenclave,
    sgx_sigstruct_t* sigstruct)
{
    oe_result_t result = OE_UNEXPECTED;

    memset(sigstruct, 0, sizeof(sgx_sigstruct_t));

    /* If sigstruct doesn't have expected header, treat enclave as unsigned */
    if (memcmp(
            ((sgx_sigstruct_t*)properties->sigstruct)->header,
            SGX_SIGSTRUCT_HEADER,
            sizeof(SGX_SIGSTRUCT_HEADER)) != 0)
    {
        /* Only debug-sign unsigned enclaves in debug mode, fail otherwise */
        if (!(properties->config.attributes & SGX_FLAGS_DEBUG))
            OE_RAISE(OE_FAILURE);

        /* Perform debug-signing with well-known debug-signing key */
        OE_CHECK(
            oe_sgx_sign_enclave(
                mrenclave,
                properties->config.attributes,
                properties->config.product_id,
                properties->config.security_version,
                OE_DEBUG_SIGN_KEY,
                OE_DEBUG_SIGN_KEY_SIZE,
                sigstruct));
    }
    else
    {
        /* Otherwise, treat enclave as signed and use its sigstruct */
        OE_CHECK(
            oe_memcpy_s(
                sigstruct,
                sizeof(sgx_sigstruct_t),
                properties->sigstruct,
                sizeof(sgx_sigstruct_t)));
    }

    result = OE_OK;

done:

    return result;
}

/* obtaining a launch token is only necessary when not using libsgx */
#if !defined(OE_USE_LIBSGX)
static oe_result_t _get_launch_token(
    const oe_sgx_enclave_properties_t* properties,
    sgx_sigstruct_t* sigstruct,
    sgx_launch_token_t* launch_token)
{
    oe_result_t result = OE_UNEXPECTED;
    AESM* aesm = NULL;

    /* Initialize the SGX attributes */
    sgx_attributes_t attributes = {0};
    attributes.flags = properties->config.attributes;
    attributes.xfrm = SGX_ATTRIBUTES_DEFAULT_XFRM;

    memset(launch_token, 0, sizeof(sgx_launch_token_t));

    /* Obtain a launch token from the AESM service */
    if (!(aesm = AESMConnect()))
        OE_RAISE(OE_FAILURE);

    OE_CHECK(
        AESMGetLaunchToken(
            aesm,
            sigstruct->enclavehash,
            sigstruct->modulus,
            &attributes,
            launch_token));

    result = OE_OK;

done:

    if (aesm)
        AESMDisconnect(aesm);

    return result;
}
#endif

oe_result_t oe_sgx_initialize_load_context(
    oe_sgx_load_context_t* context,
    oe_sgx_load_type_t type,
    uint32_t attributes)
{
    oe_result_t result = OE_UNEXPECTED;

    if (context)
        memset(context, 0, sizeof(oe_sgx_load_context_t));
    if (!context || type == OE_SGX_LOAD_TYPE_UNDEFINED)
        OE_RAISE(OE_INVALID_PARAMETER);

    /* Set attributes before checking context properties */
    context->type = type;
    context->attributes = attributes;
    context->dev = OE_SGX_NO_DEVICE_HANDLE;
#if !defined(OE_USE_LIBSGX) && defined(__linux__)
    if (type != OE_SGX_LOAD_TYPE_MEASURE &&
        !oe_sgx_is_simulation_load_context(context))
    {
        context->dev = open("/dev/isgx", O_RDWR);
        if (context->dev == OE_SGX_NO_DEVICE_HANDLE)
            OE_RAISE(OE_FAILURE);
    }
#endif

    context->state = OE_SGX_LOAD_STATE_INITIALIZED;
    result = OE_OK;

done:
    return result;
}

void oe_sgx_cleanup_load_context(oe_sgx_load_context_t* context)
{
#if !defined(OE_USE_LIBSGX) && defined(__linux__)
    if (context && context->dev != OE_SGX_NO_DEVICE_HANDLE)
        close(context->dev);
#endif
    /* Clear all fields, this also sets state to undefined */
    memset(context, 0, sizeof(oe_sgx_load_context_t));
}

oe_result_t oe_sgx_create_enclave(
    oe_sgx_load_context_t* context,
    size_t enclave_size,
    uint64_t* enclave_addr)
{
    oe_result_t result = OE_UNEXPECTED;
    void* base = NULL;
    sgx_secs_t* secs = NULL;

    if (enclave_addr)
        *enclave_addr = 0;

    if (!context || !enclave_size || !enclave_addr)
        OE_RAISE(OE_INVALID_PARAMETER);

    if (context->state != OE_SGX_LOAD_STATE_INITIALIZED)
        OE_RAISE(OE_INVALID_PARAMETER);

    /* SIZE must be a power of two */
    if (enclave_size != oe_round_u64_to_pow2(enclave_size))
        OE_RAISE(OE_INVALID_PARAMETER);

    /* Only allocate memory if we are creating an enclave in either simulation
     * mode or on Linux Kabylake machines. */
    if (context->type == OE_SGX_LOAD_TYPE_CREATE)
    {
#if defined(OE_USE_LIBSGX) || defined(_WIN32)
        if (oe_sgx_is_simulation_load_context(context))
#endif
        {
            /* Allocation memory-mapped region */
            if (!(base = _allocate_enclave_memory(enclave_size, context->dev)))
                OE_RAISE(OE_OUT_OF_MEMORY);
        }
    }

    /* Create SECS structure */
    if (!(secs = _new_secs(
              (uint64_t)base,
              enclave_size,
              oe_sgx_is_debug_load_context(context))))
        OE_RAISE(OE_OUT_OF_MEMORY);

    /* Measure this operation */
    OE_CHECK(oe_sgx_measure_create_enclave(&context->hash_context, secs));

    if (context->type == OE_SGX_LOAD_TYPE_MEASURE)
    {
        /* Use this phony base address when signing enclaves */
        base = (void*)0x0000ffff00000000;
    }
    else if (oe_sgx_is_simulation_load_context(context))
    {
        /* Simulate enclave creation */
        context->sim.addr = (void*)secs->base;
        context->sim.size = secs->size;
    }
    else
    {
#if defined(OE_USE_LIBSGX)

        uint32_t enclave_error;
        void* base = enclave_create(
            NULL, /* Let OS choose the enclave base address */
            secs->size,
            secs->size,
            ENCLAVE_TYPE_SGX1,
            (const void*)secs,
            sizeof(sgx_secs_t),
            &enclave_error);

        if (!base)
            OE_RAISE(OE_PLATFORM_ERROR);

        secs->base = (uint64_t)base;

#elif defined(__linux__)

        /* Ask the Linux SGX driver to create the enclave */
        if (sgx_ioctl_enclave_create(context->dev, secs) != 0)
            OE_RAISE(OE_IOCTL_FAILED);

#elif defined(_WIN32)

        /* Ask OS to create the enclave */
        DWORD enclave_error;
        void* base = CreateEnclave(
            GetCurrentProcess(),
            NULL, /* Let OS choose the enclave base address */
            secs->size,
            secs->size,
            ENCLAVE_TYPE_SGX,
            (const void*)secs,
            sizeof(ENCLAVE_CREATE_INFO_SGX),
            &enclave_error);

        if (!base)
            OE_RAISE(OE_PLATFORM_ERROR);

        secs->base = (uint64_t)base;

#endif
    }

    *enclave_addr = base ? (uint64_t)base : secs->base;
    context->state = OE_SGX_LOAD_STATE_ENCLAVE_CREATED;
    result = OE_OK;

done:

#if defined(__linux__)
    if (result != OE_OK && context->type == OE_SGX_LOAD_TYPE_CREATE &&
        base != NULL)
        munmap(base, enclave_size);
#endif

    if (secs)
        oe_memalign_free(secs);

    return result;
}

oe_result_t oe_sgx_load_enclave_data(
    oe_sgx_load_context_t* context,
    uint64_t base,
    uint64_t addr,
    uint64_t src,
    uint64_t flags,
    bool extend)
{
    oe_result_t result = OE_UNEXPECTED;

    if (!context || !base || !addr || !src || !flags)
        OE_RAISE(OE_INVALID_PARAMETER);

    if (context->state != OE_SGX_LOAD_STATE_ENCLAVE_CREATED)
        OE_RAISE(OE_INVALID_PARAMETER);

    /* ADDR must be page aligned */
    if (addr % OE_PAGE_SIZE)
        OE_RAISE(OE_INVALID_PARAMETER);

    /* Measure this operation */
    OE_CHECK(
        oe_sgx_measure_load_enclave_data(
            &context->hash_context, base, addr, src, flags, extend));

    if (context->type == OE_SGX_LOAD_TYPE_MEASURE)
    {
        /* EADD has no further action in measurement mode */
        result = OE_OK;
        goto done;
    }
    else if (oe_sgx_is_simulation_load_context(context))
    {
        /* Simulate enclave add page */
        /* Verify that page is within enclave boundaries */
        if ((void*)addr < context->sim.addr ||
            (uint8_t*)addr >
                (uint8_t*)context->sim.addr + context->sim.size - OE_PAGE_SIZE)
        {
            OE_RAISE(OE_FAILURE);
        }

        /* Copy page contents onto memory-mapped region */
        OE_CHECK(
            oe_memcpy_s(
                (uint8_t*)addr, OE_PAGE_SIZE, (uint8_t*)src, OE_PAGE_SIZE));

        /* Set page access permissions */
        {
            uint32_t prot =
                _make_memory_protect_param(flags, true /*simulate*/);

#if defined(__linux__)
            if (mprotect((void*)addr, OE_PAGE_SIZE, prot) != 0)
                OE_RAISE(OE_FAILURE);
#elif defined(_WIN32)
            DWORD old;
            if (!VirtualProtect((LPVOID)addr, OE_PAGE_SIZE, prot, &old))
                OE_RAISE(OE_FAILURE);
#endif
        }
    }
    else
    {
#if defined(OE_USE_LIBSGX)

        uint32_t protect =
            _make_memory_protect_param(flags, false /*not simulate*/);
        if (!extend)
            protect |= ENCLAVE_PAGE_UNVALIDATED;

        uint32_t enclave_error;
        if (enclave_load_data(
                (void*)addr,
                OE_PAGE_SIZE,
                (const void*)src,
                protect,
                &enclave_error) != OE_PAGE_SIZE)
        {
            OE_RAISE(OE_PLATFORM_ERROR);
        }

#elif defined(__linux__)

        /* Ask the Linux SGX driver to add a page to the enclave */
        if (sgx_ioctl_enclave_add_page(
                context->dev, addr, src, flags, extend) != 0)
            OE_RAISE(OE_IOCTL_FAILED);

#elif defined(_WIN32)

        /* Ask the OS to add a page to the enclave */
        SIZE_T num_bytes = 0;
        DWORD enclave_error;

        DWORD protect =
            _make_memory_protect_param(flags, false /*not simulate*/);
        if (!extend)
            protect |= PAGE_ENCLAVE_UNVALIDATED;

        if (!LoadEnclaveData(
                GetCurrentProcess(),
                (LPVOID)addr,
                (LPCVOID)src,
                OE_PAGE_SIZE,
                protect,
                NULL,
                0,
                &num_bytes,
                &enclave_error))
        {
            OE_RAISE(OE_PLATFORM_ERROR);
        }

#endif
    }

    result = OE_OK;

done:

    return result;
}

oe_result_t oe_sgx_initialize_enclave(
    oe_sgx_load_context_t* context,
    uint64_t addr,
    const oe_sgx_enclave_properties_t* properties,
    OE_SHA256* mrenclave)
{
    oe_result_t result = OE_UNEXPECTED;

    if (mrenclave)
        memset(mrenclave, 0, sizeof(OE_SHA256));

    if (!context || !addr || !properties || !mrenclave)
        OE_RAISE(OE_INVALID_PARAMETER);

    if (context->state != OE_SGX_LOAD_STATE_ENCLAVE_CREATED)
        OE_RAISE(OE_INVALID_PARAMETER);

    /* Measure this operation */
    OE_CHECK(
        oe_sgx_measure_initialize_enclave(&context->hash_context, mrenclave));

    /* EINIT has no further action in measurement/simulation mode */
    if (context->type == OE_SGX_LOAD_TYPE_CREATE &&
        !oe_sgx_is_simulation_load_context(context))
    {
        /* Get a debug sigstruct for MRENCLAVE if necessary */
        sgx_sigstruct_t sigstruct;
        OE_CHECK(_get_sig_struct(properties, mrenclave, &sigstruct));

#if defined(OE_USE_LIBSGX)

        uint32_t enclave_error = 0;
        if (!enclave_initialize(
                (void*)addr,
                (const void*)&sigstruct,
                sizeof(sgx_sigstruct_t),
                &enclave_error))
            OE_RAISE(OE_PLATFORM_ERROR);
        if (enclave_error != 0)
            OE_RAISE(OE_PLATFORM_ERROR);
#else
        /* If not using libsgx, get a launch token from the AESM service */
        sgx_launch_token_t launch_token;
        OE_CHECK(_get_launch_token(properties, &sigstruct, &launch_token));

#if defined(__linux__)

        /* Ask the Linux SGX driver to initialize the enclave */
        if (sgx_ioctl_enclave_init(
                context->dev,
                addr,
                (uint64_t)&sigstruct,
                (uint64_t)&launch_token) != 0)
            OE_RAISE(OE_IOCTL_FAILED);

#elif defined(_WIN32)

        OE_STATIC_ASSERT(
            OE_FIELD_SIZE(ENCLAVE_INIT_INFO_SGX, SigStruct) ==
            sizeof(sigstruct));
        OE_STATIC_ASSERT(
            OE_FIELD_SIZE(ENCLAVE_INIT_INFO_SGX, EInitToken) <=
            sizeof(launch_token));

        /* Ask the OS to initialize the enclave */
        DWORD enclave_error;
        ENCLAVE_INIT_INFO_SGX info = {{0}};

        OE_CHECK(
            oe_memcpy_s(
                &info.SigStruct,
                sizeof(info.SigStruct),
                (void*)&sigstruct,
                sizeof(sigstruct)));
        OE_CHECK(
            oe_memcpy_s(
                &info.EInitToken,
                sizeof(info.EInitToken),
                (void*)&launch_token,
                sizeof(launch_token)));

        if (!InitializeEnclave(
                GetCurrentProcess(),
                (LPVOID)addr,
                &info,
                sizeof(info),
                &enclave_error))
        {
            OE_RAISE(OE_PLATFORM_ERROR);
        }
#endif
#endif
    }

    context->state = OE_SGX_LOAD_STATE_ENCLAVE_INITIALIZED;
    result = OE_OK;

done:

    return result;
}

oe_result_t oe_sgx_delete_enclave(oe_enclave_t* enclave)
{
    oe_result_t result = OE_UNEXPECTED;

    if (!enclave)
        OE_RAISE(OE_INVALID_PARAMETER);

#if defined(__linux__)

/* FLC Linux needs to call `enclave_delete` in SGX mode. */
#if defined(OE_USE_LIBSGX)
    if (!enclave->simulate)
    {
        uint32_t enclave_error = 0;
        if (!enclave_delete((void*)enclave->addr, &enclave_error))
            OE_RAISE(OE_PLATFORM_ERROR);
        if (enclave_error != 0)
            OE_RAISE(OE_PLATFORM_ERROR);
    }
    else /* FLC simulation mode needs to munmap. */
#endif
    {
        /* Non-FLC Linux and simulation mode both allocate memory. */
        munmap((void*)enclave->addr, enclave->size);
    }

#elif defined(_WIN32)
    /* SGX enclaves can be freed with `VirtualFree` with the `MEM_RELEASE`
     * flag. We can't do this for simulation mode because `MEM_RELEASE`
     * requires `enclave->addr` to be the same one returned by `VirtualAlloc`,
     * which is often not the case due to the enclave address alignment
     * requirements. We use `MEM_DECOMMIT` instead. */
    if (!enclave->simulate)
        VirtualFree((void*)enclave->addr, 0, MEM_RELEASE);
    else
        VirtualFree((void*)enclave->addr, enclave->size, MEM_DECOMMIT);

#endif

    result = OE_OK;

done:

    return result;
}
