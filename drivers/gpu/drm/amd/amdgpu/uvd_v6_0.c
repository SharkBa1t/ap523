/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Christian König <christian.koenig@amd.com>
 */

#include <linux/firmware.h>
#include <drm/drmP.h>
#include "amdgpu.h"
#include "amdgpu_uvd.h"
#include "vid.h"
#include "uvd/uvd_6_0_d.h"
#include "uvd/uvd_6_0_sh_mask.h"
#include "oss/oss_2_0_d.h"
#include "oss/oss_2_0_sh_mask.h"

static void uvd_v6_0_set_ring_funcs(struct amdgpu_device *adev);
static void uvd_v6_0_set_irq_funcs(struct amdgpu_device *adev);
static int uvd_v6_0_start(struct amdgpu_device *adev);
static void uvd_v6_0_stop(struct amdgpu_device *adev);

/**
 * uvd_v6_0_ring_get_rptr - get read pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware read pointer
 */
static uint32_t uvd_v6_0_ring_get_rptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_RPTR);
}

/**
 * uvd_v6_0_ring_get_wptr - get write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware write pointer
 */
static uint32_t uvd_v6_0_ring_get_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_WPTR);
}

/**
 * uvd_v6_0_ring_set_wptr - set write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Commits the write pointer to the hardware
 */
static void uvd_v6_0_ring_set_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	WREG32(mmUVD_RBC_RB_WPTR, ring->wptr);
}

static int uvd_v6_0_early_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	uvd_v6_0_set_ring_funcs(adev);
	uvd_v6_0_set_irq_funcs(adev);

	return 0;
}

static int uvd_v6_0_sw_init(void *handle)
{
	struct amdgpu_ring *ring;
	int r;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	/* UVD TRAP */
	r = amdgpu_irq_add_id(adev, 124, &adev->uvd.irq);
	if (r)
		return r;

	r = amdgpu_uvd_sw_init(adev);
	if (r)
		return r;

	r = amdgpu_uvd_resume(adev);
	if (r)
		return r;

	ring = &adev->uvd.ring;
	sprintf(ring->name, "uvd");
	r = amdgpu_ring_init(adev, ring, 4096, CP_PACKET2, 0xf,
			     &adev->uvd.irq, 0, AMDGPU_RING_TYPE_UVD);

	return r;
}

static int uvd_v6_0_sw_fini(void *handle)
{
	int r;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	r = amdgpu_uvd_suspend(adev);
	if (r)
		return r;

	r = amdgpu_uvd_sw_fini(adev);
	if (r)
		return r;

	return r;
}

/**
 * uvd_v6_0_hw_init - start and test UVD block
 *
 * @adev: amdgpu_device pointer
 *
 * Initialize the hardware, boot up the VCPU and do some testing
 */
static int uvd_v6_0_hw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_ring *ring = &adev->uvd.ring;
	uint32_t tmp;
	int r;

	r = uvd_v6_0_start(adev);
	if (r)
		goto done;

	ring->ready = true;
	r = amdgpu_ring_test_ring(ring);
	if (r) {
		ring->ready = false;
		goto done;
	}

	r = amdgpu_ring_lock(ring, 10);
	if (r) {
		DRM_ERROR("amdgpu: ring failed to lock UVD ring (%d).\n", r);
		goto done;
	}

	tmp = PACKET0(mmUVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	tmp = PACKET0(mmUVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	tmp = PACKET0(mmUVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	/* Clear timeout status bits */
	amdgpu_ring_write(ring, PACKET0(mmUVD_SEMA_TIMEOUT_STATUS, 0));
	amdgpu_ring_write(ring, 0x8);

	amdgpu_ring_write(ring, PACKET0(mmUVD_SEMA_CNTL, 0));
	amdgpu_ring_write(ring, 3);

	amdgpu_ring_unlock_commit(ring);

done:
	if (!r)
		DRM_INFO("UVD initialized successfully.\n");

	return r;
}

/**
 * uvd_v6_0_hw_fini - stop the hardware block
 *
 * @adev: amdgpu_device pointer
 *
 * Stop the UVD block, mark ring as not ready any more
 */
static int uvd_v6_0_hw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_ring *ring = &adev->uvd.ring;

	uvd_v6_0_stop(adev);
	ring->ready = false;

	return 0;
}

static int uvd_v6_0_suspend(void *handle)
{
	int r;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	/* Skip this for APU for now */
	if (!(adev->flags & AMD_IS_APU)) {
		r = amdgpu_uvd_suspend(adev);
		if (r)
			return r;
	}
	r = uvd_v6_0_hw_fini(adev);
	if (r)
		return r;

	return r;
}

static int uvd_v6_0_resume(void *handle)
{
	int r;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	/* Skip this for APU for now */
	if (!(adev->flags & AMD_IS_APU)) {
		r = amdgpu_uvd_resume(adev);
		if (r)
			return r;
	}
	r = uvd_v6_0_hw_init(adev);
	if (r)
		return r;

	return r;
}

/**
 * uvd_v6_0_mc_resume - memory controller programming
 *
 * @adev: amdgpu_device pointer
 *
 * Let the UVD memory controller know it's offsets
 */
static void uvd_v6_0_mc_resume(struct amdgpu_device *adev)
{
	uint64_t offset;
	uint32_t size;

	/* programm memory controller bits 0-27 */
	WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW,
			lower_32_bits(adev->uvd.gpu_addr));
	WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH,
			upper_32_bits(adev->uvd.gpu_addr));

	offset = AMDGPU_UVD_FIRMWARE_OFFSET;
	size = AMDGPU_GPU_PAGE_ALIGN(adev->uvd.fw->size + 4);
	WREG32(mmUVD_VCPU_CACHE_OFFSET0, offset >> 3);
	WREG32(mmUVD_VCPU_CACHE_SIZE0, size);

	offset += size;
	size = AMDGPU_UVD_STACK_SIZE;
	WREG32(mmUVD_VCPU_CACHE_OFFSET1, offset >> 3);
	WREG32(mmUVD_VCPU_CACHE_SIZE1, size);

	offset += size;
	size = AMDGPU_UVD_HEAP_SIZE;
	WREG32(mmUVD_VCPU_CACHE_OFFSET2, offset >> 3);
	WREG32(mmUVD_VCPU_CACHE_SIZE2, size);
}

/**
 * uvd_v6_0_start - start UVD block
 *
 * @adev: amdgpu_device pointer
 *
 * Setup and start the UVD block
 */
static int uvd_v6_0_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring = &adev->uvd.ring;
	uint32_t rb_bufsz, tmp;
	uint32_t lmi_swap_cntl;
	uint32_t mp_swap_cntl;
	int i, j, r;

	/*disable DPG */
	WREG32_P(mmUVD_POWER_STATUS, 0, ~(1 << 2));

	/* disable byte swapping */
	lmi_swap_cntl = 0;
	mp_swap_cntl = 0;

	uvd_v6_0_mc_resume(adev);

	/* disable clock gating */
	WREG32(mmUVD_CGC_GATE, 0);

	/* disable interupt */
	WREG32_P(mmUVD_MASTINT_EN, 0, ~(1 << 1));

	/* stall UMC and register bus before resetting VCPU */
	WREG32_P(mmUVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
	mdelay(1);

	/* put LMI, VCPU, RBC etc... into reset */
	WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__LMI_SOFT_RESET_MASK |
		UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK | UVD_SOFT_RESET__LBSI_SOFT_RESET_MASK |
		UVD_SOFT_RESET__RBC_SOFT_RESET_MASK | UVD_SOFT_RESET__CSM_SOFT_RESET_MASK |
		UVD_SOFT_RESET__CXW_SOFT_RESET_MASK | UVD_SOFT_RESET__TAP_SOFT_RESET_MASK |
		UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);
	mdelay(5);

	/* take UVD block out of reset */
	WREG32_P(mmSRBM_SOFT_RESET, 0, ~SRBM_SOFT_RESET__SOFT_RESET_UVD_MASK);
	mdelay(5);

	/* initialize UVD memory controller */
	WREG32(mmUVD_LMI_CTRL, 0x40 | (1 << 8) | (1 << 13) |
			     (1 << 21) | (1 << 9) | (1 << 20));

#ifdef __BIG_ENDIAN
	/* swap (8 in 32) RB and IB */
	lmi_swap_cntl = 0xa;
	mp_swap_cntl = 0;
#endif
	WREG32(mmUVD_LMI_SWAP_CNTL, lmi_swap_cntl);
	WREG32(mmUVD_MP_SWAP_CNTL, mp_swap_cntl);

	WREG32(mmUVD_MPC_SET_MUXA0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXA1, 0x0);
	WREG32(mmUVD_MPC_SET_MUXB0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXB1, 0x0);
	WREG32(mmUVD_MPC_SET_ALU, 0);
	WREG32(mmUVD_MPC_SET_MUX, 0x88);

	/* take all subblocks out of reset, except VCPU */
	WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
	mdelay(5);

	/* enable VCPU clock */
	WREG32(mmUVD_VCPU_CNTL,  1 << 9);

	/* enable UMC */
	WREG32_P(mmUVD_LMI_CTRL2, 0, ~(1 << 8));

	/* boot up the VCPU */
	WREG32(mmUVD_SOFT_RESET, 0);
	mdelay(10);

	for (i = 0; i < 10; ++i) {
		uint32_t status;

		for (j = 0; j < 100; ++j) {
			status = RREG32(mmUVD_STATUS);
			if (status & 2)
				break;
			mdelay(10);
		}
		r = 0;
		if (status & 2)
			break;

		DRM_ERROR("UVD not responding, trying to reset the VCPU!!!\n");
		WREG32_P(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK,
				~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
		mdelay(10);
		WREG32_P(mmUVD_SOFT_RESET, 0,
			 ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
		mdelay(10);
		r = -1;
	}

	if (r) {
		DRM_ERROR("UVD not responding, giving up!!!\n");
		return r;
	}
	/* enable master interrupt */
	WREG32_P(mmUVD_MASTINT_EN, 3 << 1, ~(3 << 1));

	/* clear the bit 4 of UVD_STATUS */
	WREG32_P(mmUVD_STATUS, 0, ~(2 << 1));

	rb_bufsz = order_base_2(ring->ring_size);
	tmp = 0;
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_BUFSZ, rb_bufsz);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_BLKSZ, 1);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_FETCH, 1);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_WPTR_POLL_EN, 0);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_UPDATE, 1);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_RPTR_WR_EN, 1);
	/* force RBC into idle state */
	WREG32(mmUVD_RBC_RB_CNTL, tmp);

	/* set the write pointer delay */
	WREG32(mmUVD_RBC_RB_WPTR_CNTL, 0);

	/* set the wb address */
	WREG32(mmUVD_RBC_RB_RPTR_ADDR, (upper_32_bits(ring->gpu_addr) >> 2));

	/* programm the RB_BASE for ring buffer */
	WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_LOW,
			lower_32_bits(ring->gpu_addr));
	WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH,
			upper_32_bits(ring->gpu_addr));

	/* Initialize the ring buffer's read and write pointers */
	WREG32(mmUVD_RBC_RB_RPTR, 0);

	ring->wptr = RREG32(mmUVD_RBC_RB_RPTR);
	WREG32(mmUVD_RBC_RB_WPTR, ring->wptr);

	WREG32_P(mmUVD_RBC_RB_CNTL, 0, ~UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK);

	return 0;
}

/**
 * uvd_v6_0_stop - stop UVD block
 *
 * @adev: amdgpu_device pointer
 *
 * stop the UVD block
 */
static void uvd_v6_0_stop(struct amdgpu_device *adev)
{
	/* force RBC into idle state */
	WREG32(mmUVD_RBC_RB_CNTL, 0x11010101);

	/* Stall UMC and register bus before resetting VCPU */
	WREG32_P(mmUVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
	mdelay(1);

	/* put VCPU into reset */
	WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
	mdelay(5);

	/* disable VCPU clock */
	WREG32(mmUVD_VCPU_CNTL, 0x0);

	/* Unstall UMC and register bus */
	WREG32_P(mmUVD_LMI_CTRL2, 0, ~(1 << 8));
}

/**
 * uvd_v6_0_ring_emit_fence - emit an fence & trap command
 *
 * @ring: amdgpu_ring pointer
 * @fence: fence to emit
 *
 * Write a fence and a trap command to the ring.
 */
static void uvd_v6_0_ring_emit_fence(struct amdgpu_ring *ring, u64 addr, u64 seq,
				     unsigned flags)
{
	WARN_ON(flags & AMDGPU_FENCE_FLAG_64BIT);

	amdgpu_ring_write(ring, PACKET0(mmUVD_CONTEXT_ID, 0));
	amdgpu_ring_write(ring, seq);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, addr & 0xffffffff);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, upper_32_bits(addr) & 0xff);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 2);
}

/**
 * uvd_v6_0_ring_emit_semaphore - emit semaphore command
 *
 * @ring: amdgpu_ring pointer
 * @semaphore: semaphore to emit commands for
 * @emit_wait: true if we should emit a wait command
 *
 * Emit a semaphore command (either wait or signal) to the UVD ring.
 */
static bool uvd_v6_0_ring_emit_semaphore(struct amdgpu_ring *ring,
					 struct amdgpu_semaphore *semaphore,
					 bool emit_wait)
{
	uint64_t addr = semaphore->gpu_addr;

	amdgpu_ring_write(ring, PACKET0(mmUVD_SEMA_ADDR_LOW, 0));
	amdgpu_ring_write(ring, (addr >> 3) & 0x000FFFFF);

	amdgpu_ring_write(ring, PACKET0(mmUVD_SEMA_ADDR_HIGH, 0));
	amdgpu_ring_write(ring, (addr >> 23) & 0x000FFFFF);

	amdgpu_ring_write(ring, PACKET0(mmUVD_SEMA_CMD, 0));
	amdgpu_ring_write(ring, 0x80 | (emit_wait ? 1 : 0));

	return true;
}

/**
 * uvd_v6_0_ring_test_ring - register write test
 *
 * @ring: amdgpu_ring pointer
 *
 * Test if we can successfully write to the context register
 */
static int uvd_v6_0_ring_test_ring(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	WREG32(mmUVD_CONTEXT_ID, 0xCAFEDEAD);
	r = amdgpu_ring_lock(ring, 3);
	if (r) {
		DRM_ERROR("amdgpu: cp failed to lock ring %d (%d).\n",
			  ring->idx, r);
		return r;
	}
	amdgpu_ring_write(ring, PACKET0(mmUVD_CONTEXT_ID, 0));
	amdgpu_ring_write(ring, 0xDEADBEEF);
	amdgpu_ring_unlock_commit(ring);
	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(mmUVD_CONTEXT_ID);
		if (tmp == 0xDEADBEEF)
			break;
		DRM_UDELAY(1);
	}

	if (i < adev->usec_timeout) {
		DRM_INFO("ring test on %d succeeded in %d usecs\n",
			 ring->idx, i);
	} else {
		DRM_ERROR("amdgpu: ring %d test failed (0x%08X)\n",
			  ring->idx, tmp);
		r = -EINVAL;
	}
	return r;
}

/**
 * uvd_v6_0_ring_emit_ib - execute indirect buffer
 *
 * @ring: amdgpu_ring pointer
 * @ib: indirect buffer to execute
 *
 * Write ring commands to execute the indirect buffer
 */
static void uvd_v6_0_ring_emit_ib(struct amdgpu_ring *ring,
				  struct amdgpu_ib *ib)
{
	amdgpu_ring_write(ring, PACKET0(mmUVD_LMI_RBC_IB_64BIT_BAR_LOW, 0));
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, PACKET0(mmUVD_LMI_RBC_IB_64BIT_BAR_HIGH, 0));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, PACKET0(mmUVD_RBC_IB_SIZE, 0));
	amdgpu_ring_write(ring, ib->length_dw);
}

/**
 * uvd_v6_0_ring_test_ib - test ib execution
 *
 * @ring: amdgpu_ring pointer
 *
 * Test if we can successfully execute an IB
 */
static int uvd_v6_0_ring_test_ib(struct amdgpu_ring *ring)
{
	struct fence *fence = NULL;
	int r;

	r = amdgpu_uvd_get_create_msg(ring, 1, NULL);
	if (r) {
		DRM_ERROR("amdgpu: failed to get create msg (%d).\n", r);
		goto error;
	}

	r = amdgpu_uvd_get_destroy_msg(ring, 1, &fence);
	if (r) {
		DRM_ERROR("amdgpu: failed to get destroy ib (%d).\n", r);
		goto error;
	}

	r = fence_wait(fence, false);
	if (r) {
		DRM_ERROR("amdgpu: fence wait failed (%d).\n", r);
		goto error;
	}
	DRM_INFO("ib test on ring %d succeeded\n",  ring->idx);
error:
	fence_put(fence);
	return r;
}

static bool uvd_v6_0_is_idle(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	return !(RREG32(mmSRBM_STATUS) & SRBM_STATUS__UVD_BUSY_MASK);
}

static int uvd_v6_0_wait_for_idle(void *handle)
{
	unsigned i;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	for (i = 0; i < adev->usec_timeout; i++) {
		if (!(RREG32(mmSRBM_STATUS) & SRBM_STATUS__UVD_BUSY_MASK))
			return 0;
	}
	return -ETIMEDOUT;
}

static int uvd_v6_0_soft_reset(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	uvd_v6_0_stop(adev);

	WREG32_P(mmSRBM_SOFT_RESET, SRBM_SOFT_RESET__SOFT_RESET_UVD_MASK,
			~SRBM_SOFT_RESET__SOFT_RESET_UVD_MASK);
	mdelay(5);

	return uvd_v6_0_start(adev);
}

static void uvd_v6_0_print_status(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	dev_info(adev->dev, "UVD 6.0 registers\n");
	dev_info(adev->dev, "  UVD_SEMA_ADDR_LOW=0x%08X\n",
		 RREG32(mmUVD_SEMA_ADDR_LOW));
	dev_info(adev->dev, "  UVD_SEMA_ADDR_HIGH=0x%08X\n",
		 RREG32(mmUVD_SEMA_ADDR_HIGH));
	dev_info(adev->dev, "  UVD_SEMA_CMD=0x%08X\n",
		 RREG32(mmUVD_SEMA_CMD));
	dev_info(adev->dev, "  UVD_GPCOM_VCPU_CMD=0x%08X\n",
		 RREG32(mmUVD_GPCOM_VCPU_CMD));
	dev_info(adev->dev, "  UVD_GPCOM_VCPU_DATA0=0x%08X\n",
		 RREG32(mmUVD_GPCOM_VCPU_DATA0));
	dev_info(adev->dev, "  UVD_GPCOM_VCPU_DATA1=0x%08X\n",
		 RREG32(mmUVD_GPCOM_VCPU_DATA1));
	dev_info(adev->dev, "  UVD_ENGINE_CNTL=0x%08X\n",
		 RREG32(mmUVD_ENGINE_CNTL));
	dev_info(adev->dev, "  UVD_UDEC_ADDR_CONFIG=0x%08X\n",
		 RREG32(mmUVD_UDEC_ADDR_CONFIG));
	dev_info(adev->dev, "  UVD_UDEC_DB_ADDR_CONFIG=0x%08X\n",
		 RREG32(mmUVD_UDEC_DB_ADDR_CONFIG));
	dev_info(adev->dev, "  UVD_UDEC_DBW_ADDR_CONFIG=0x%08X\n",
		 RREG32(mmUVD_UDEC_DBW_ADDR_CONFIG));
	dev_info(adev->dev, "  UVD_SEMA_CNTL=0x%08X\n",
		 RREG32(mmUVD_SEMA_CNTL));
	dev_info(adev->dev, "  UVD_LMI_EXT40_ADDR=0x%08X\n",
		 RREG32(mmUVD_LMI_EXT40_ADDR));
	dev_info(adev->dev, "  UVD_CTX_INDEX=0x%08X\n",
		 RREG32(mmUVD_CTX_INDEX));
	dev_info(adev->dev, "  UVD_CTX_DATA=0x%08X\n",
		 RREG32(mmUVD_CTX_DATA));
	dev_info(adev->dev, "  UVD_CGC_GATE=0x%08X\n",
		 RREG32(mmUVD_CGC_GATE));
	dev_info(adev->dev, "  UVD_CGC_CTRL=0x%08X\n",
		 RREG32(mmUVD_CGC_CTRL));
	dev_info(adev->dev, "  UVD_LMI_CTRL2=0x%08X\n",
		 RREG32(mmUVD_LMI_CTRL2));
	dev_info(adev->dev, "  UVD_MASTINT_EN=0x%08X\n",
		 RREG32(mmUVD_MASTINT_EN));
	dev_info(adev->dev, "  UVD_LMI_ADDR_EXT=0x%08X\n",
		 RREG32(mmUVD_LMI_ADDR_EXT));
	dev_info(adev->dev, "  UVD_LMI_CTRL=0x%08X\n",
		 RREG32(mmUVD_LMI_CTRL));
	dev_info(adev->dev, "  UVD_LMI_SWAP_CNTL=0x%08X\n",
		 RREG32(mmUVD_LMI_SWAP_CNTL));
	dev_info(adev->dev, "  UVD_MP_SWAP_CNTL=0x%08X\n",
		 RREG32(mmUVD_MP_SWAP_CNTL));
	dev_info(adev->dev, "  UVD_MPC_SET_MUXA0=0x%08X\n",
		 RREG32(mmUVD_MPC_SET_MUXA0));
	dev_info(adev->dev, "  UVD_MPC_SET_MUXA1=0x%08X\n",
		 RREG32(mmUVD_MPC_SET_MUXA1));
	dev_info(adev->dev, "  UVD_MPC_SET_MUXB0=0x%08X\n",
		 RREG32(mmUVD_MPC_SET_MUXB0));
	dev_info(adev->dev, "  UVD_MPC_SET_MUXB1=0x%08X\n",
		 RREG32(mmUVD_MPC_SET_MUXB1));
	dev_info(adev->dev, "  UVD_MPC_SET_MUX=0x%08X\n",
		 RREG32(mmUVD_MPC_SET_MUX));
	dev_info(adev->dev, "  UVD_MPC_SET_ALU=0x%08X\n",
		 RREG32(mmUVD_MPC_SET_ALU));
	dev_info(adev->dev, "  UVD_VCPU_CACHE_OFFSET0=0x%08X\n",
		 RREG32(mmUVD_VCPU_CACHE_OFFSET0));
	dev_info(adev->dev, "  UVD_VCPU_CACHE_SIZE0=0x%08X\n",
		 RREG32(mmUVD_VCPU_CACHE_SIZE0));
	dev_info(adev->dev, "  UVD_VCPU_CACHE_OFFSET1=0x%08X\n",
		 RREG32(mmUVD_VCPU_CACHE_OFFSET1));
	dev_info(adev->dev, "  UVD_VCPU_CACHE_SIZE1=0x%08X\n",
		 RREG32(mmUVD_VCPU_CACHE_SIZE1));
	dev_info(adev->dev, "  UVD_VCPU_CACHE_OFFSET2=0x%08X\n",
		 RREG32(mmUVD_VCPU_CACHE_OFFSET2));
	dev_info(adev->dev, "  UVD_VCPU_CACHE_SIZE2=0x%08X\n",
		 RREG32(mmUVD_VCPU_CACHE_SIZE2));
	dev_info(adev->dev, "  UVD_VCPU_CNTL=0x%08X\n",
		 RREG32(mmUVD_VCPU_CNTL));
	dev_info(adev->dev, "  UVD_SOFT_RESET=0x%08X\n",
		 RREG32(mmUVD_SOFT_RESET));
	dev_info(adev->dev, "  UVD_RBC_IB_SIZE=0x%08X\n",
		 RREG32(mmUVD_RBC_IB_SIZE));
	dev_info(adev->dev, "  UVD_RBC_RB_RPTR=0x%08X\n",
		 RREG32(mmUVD_RBC_RB_RPTR));
	dev_info(adev->dev, "  UVD_RBC_RB_WPTR=0x%08X\n",
		 RREG32(mmUVD_RBC_RB_WPTR));
	dev_info(adev->dev, "  UVD_RBC_RB_WPTR_CNTL=0x%08X\n",
		 RREG32(mmUVD_RBC_RB_WPTR_CNTL));
	dev_info(adev->dev, "  UVD_RBC_RB_CNTL=0x%08X\n",
		 RREG32(mmUVD_RBC_RB_CNTL));
	dev_info(adev->dev, "  UVD_STATUS=0x%08X\n",
		 RREG32(mmUVD_STATUS));
	dev_info(adev->dev, "  UVD_SEMA_TIMEOUT_STATUS=0x%08X\n",
		 RREG32(mmUVD_SEMA_TIMEOUT_STATUS));
	dev_info(adev->dev, "  UVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL=0x%08X\n",
		 RREG32(mmUVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL));
	dev_info(adev->dev, "  UVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL=0x%08X\n",
		 RREG32(mmUVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL));
	dev_info(adev->dev, "  UVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL=0x%08X\n",
		 RREG32(mmUVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL));
	dev_info(adev->dev, "  UVD_CONTEXT_ID=0x%08X\n",
		 RREG32(mmUVD_CONTEXT_ID));
}

static int uvd_v6_0_set_interrupt_state(struct amdgpu_device *adev,
					struct amdgpu_irq_src *source,
					unsigned type,
					enum amdgpu_interrupt_state state)
{
	// TODO
	return 0;
}

static int uvd_v6_0_process_interrupt(struct amdgpu_device *adev,
				      struct amdgpu_irq_src *source,
				      struct amdgpu_iv_entry *entry)
{
	DRM_DEBUG("IH: UVD TRAP\n");
	amdgpu_fence_process(&adev->uvd.ring);
	return 0;
}

static int uvd_v6_0_set_clockgating_state(void *handle,
					  enum amd_clockgating_state state)
{
	return 0;
}

static int uvd_v6_0_set_powergating_state(void *handle,
					  enum amd_powergating_state state)
{
	/* This doesn't actually powergate the UVD block.
	 * That's done in the dpm code via the SMC.  This
	 * just re-inits the block as necessary.  The actual
	 * gating still happens in the dpm code.  We should
	 * revisit this when there is a cleaner line between
	 * the smc and the hw blocks
	 */
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	if (state == AMD_PG_STATE_GATE) {
		uvd_v6_0_stop(adev);
		return 0;
	} else {
		return uvd_v6_0_start(adev);
	}
}

const struct amd_ip_funcs uvd_v6_0_ip_funcs = {
	.early_init = uvd_v6_0_early_init,
	.late_init = NULL,
	.sw_init = uvd_v6_0_sw_init,
	.sw_fini = uvd_v6_0_sw_fini,
	.hw_init = uvd_v6_0_hw_init,
	.hw_fini = uvd_v6_0_hw_fini,
	.suspend = uvd_v6_0_suspend,
	.resume = uvd_v6_0_resume,
	.is_idle = uvd_v6_0_is_idle,
	.wait_for_idle = uvd_v6_0_wait_for_idle,
	.soft_reset = uvd_v6_0_soft_reset,
	.print_status = uvd_v6_0_print_status,
	.set_clockgating_state = uvd_v6_0_set_clockgating_state,
	.set_powergating_state = uvd_v6_0_set_powergating_state,
};

static const struct amdgpu_ring_funcs uvd_v6_0_ring_funcs = {
	.get_rptr = uvd_v6_0_ring_get_rptr,
	.get_wptr = uvd_v6_0_ring_get_wptr,
	.set_wptr = uvd_v6_0_ring_set_wptr,
	.parse_cs = amdgpu_uvd_ring_parse_cs,
	.emit_ib = uvd_v6_0_ring_emit_ib,
	.emit_fence = uvd_v6_0_ring_emit_fence,
	.emit_semaphore = uvd_v6_0_ring_emit_semaphore,
	.test_ring = uvd_v6_0_ring_test_ring,
	.test_ib = uvd_v6_0_ring_test_ib,
	.is_lockup = amdgpu_ring_test_lockup,
	.insert_nop = amdgpu_ring_insert_nop,
};

static void uvd_v6_0_set_ring_funcs(struct amdgpu_device *adev)
{
	adev->uvd.ring.funcs = &uvd_v6_0_ring_funcs;
}

static const struct amdgpu_irq_src_funcs uvd_v6_0_irq_funcs = {
	.set = uvd_v6_0_set_interrupt_state,
	.process = uvd_v6_0_process_interrupt,
};

static void uvd_v6_0_set_irq_funcs(struct amdgpu_device *adev)
{
	adev->uvd.irq.num_types = 1;
	adev->uvd.irq.funcs = &uvd_v6_0_irq_funcs;
}
