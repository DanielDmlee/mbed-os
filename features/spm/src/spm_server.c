/* Copyright (c) 2017 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include "mbed_toolchain.h"
#include "spm_server.h"
#include "spm_internal.h"
#include "spm_panic.h"
#include "handles_manager.h"

#define PSA_SIG_DOORBELL_POS    3                           // Bit position for PSA_DOORBELL signal
#define PSA_SIG_DOORBELL_MSK    (1 << PSA_SIG_DOORBELL_POS) // Mask for PSA_DOORBELL signal


extern spm_t g_spm;

static inline partition_t *partition_get_by_pid(int32_t partition_id)
{
    for (uint32_t i = 0; i < g_spm.partition_count; ++i) {
        if (g_spm.partitions[i].partition_id == partition_id) {
            return &(g_spm.partitions[i]);
        }
    }

    return NULL;
}

static inline error_t spm_msg_handle_create(void *handle_mem, int32_t friend_pid, psa_handle_t *handle)
{
    return psa_hndl_mgr_handle_create(&(g_spm.messages_handle_mgr), handle_mem, friend_pid, handle);
}

static inline void spm_msg_handle_destroy(psa_handle_t handle)
{
    psa_hndl_mgr_handle_destroy(&(g_spm.messages_handle_mgr), handle);
}

static inline void spm_msg_handle_get_mem(psa_handle_t handle, active_msg_t **handle_mem)
{
    psa_hndl_mgr_handle_get_mem(&(g_spm.messages_handle_mgr), handle, (void **)handle_mem);
}

static uint32_t psa_wait(bool wait_any, uint32_t bitmask, uint32_t timeout)
{
    partition_t *curr_partition = active_partition_get();
    SPM_ASSERT(NULL != curr_partition); // active thread in SPM must be in partition DB

    uint32_t flags_all = curr_partition->flags_sf | curr_partition->flags_interrupts;

    // In case we're waiting for any signal the bitmask must contain all the flags, otherwise
    // we should be waiting for a subset of interrupt signals.
    if (wait_any) {
        bitmask = flags_all;
    } else {
        // Make sure the interrupt mask contains only a subset of interrupt signal mask.
        if (bitmask != (curr_partition->flags_interrupts & bitmask)) {
            spm_panic("%s - interrupt mask 0x%x must have only bits from 0x%x!\n",
                __func__, bitmask, curr_partition->flags_interrupts);
        }
    }
    
    uint32_t asserted_signals = osThreadFlagsWait(
        bitmask,
        osFlagsWaitAny | osFlagsNoClear,
        (PSA_WAIT_BLOCK == timeout) ? osWaitForever : timeout
        );

    // Asserted_signals must be a subset of the supported SF and interrupt signals.
    SPM_ASSERT((asserted_signals == (asserted_signals & flags_all)) ||
               ((PSA_WAIT_BLOCK != timeout) && (osFlagsErrorTimeout == asserted_signals)));

    return (osFlagsErrorTimeout == asserted_signals) ? 0 : asserted_signals;
}

uint32_t psa_wait_any(uint32_t timeout)
{
    return psa_wait(true, 0, timeout);
}

uint32_t psa_wait_interrupt(uint32_t interrupt_mask, uint32_t timeout)
{
    return psa_wait(false, interrupt_mask, timeout);
}

void psa_get(psa_signal_t signum, psa_msg_t *msg)
{
    if (NULL == msg) {
        spm_panic("%s - 'msg' is NULL!\n", __func__);
    }

    // TODO: assert memory range [msg, msg + sizeof(psa_msg_t)] is writable by the caller

    partition_t *curr_partition = active_partition_get();
    SPM_ASSERT(NULL != curr_partition); // active thread in SPM must be in partition DB

    // signum must be ONLY ONE of the bits of curr_partition->flags_sf
    bool is_one_bit = ((signum != 0) && !(signum & (signum - 1)));
    if (!is_one_bit || !(signum & (curr_partition->flags_sf | curr_partition->flags_interrupts))) {
        spm_panic(
            "%s - signum 0x%x must have only 1 bit ON and must be a subset of 0x%x!\n",
            __func__,
            signum,
            curr_partition->flags_sf | curr_partition->flags_interrupts
            );
    }

    active_msg_t *active_msg = &(curr_partition->active_msg);
    SPM_ASSERT((active_msg->type > PSA_IPC_MSG_TYPE_INVALID) &&
               (active_msg->type <= PSA_IPC_MSG_TYPE_MAX));

    SPM_ASSERT((active_msg->channel->rhandle == NULL) ||
               (active_msg->type != PSA_IPC_MSG_TYPE_CONNECT));

    PARTITION_STATE_ASSERT(curr_partition, PARTITION_STATE_ACTIVE);

    if (curr_partition->msg_handle == PSA_NULL_HANDLE) {
        psa_error_t status = spm_msg_handle_create(
                active_msg,
                curr_partition->partition_id,
                &(curr_partition->msg_handle)
                );
        SPM_ASSERT(PSA_SUCCESS == status);
        PSA_UNUSED(status);
    }

    msg->type = active_msg->type;
    msg->handle = curr_partition->msg_handle;
    msg->rhandle = active_msg->channel->rhandle;
    msg->size = active_msg->tx_size;
    msg->response_size = active_msg->rx_size;
}

size_t psa_read(psa_handle_t msg_handle, size_t offset, void *buffer, size_t len)
{
    SPM_ASSERT(buffer != NULL);
    SPM_ASSERT(offset <= UINT32_MAX - len);

    active_msg_t *active_msg = NULL;
    spm_msg_handle_get_mem(msg_handle, &active_msg);

    SPM_ASSERT((active_msg->type > PSA_IPC_MSG_TYPE_INVALID) &&
               (active_msg->type <= PSA_IPC_MSG_TYPE_MAX));

    PARTITION_STATE_ASSERT(active_msg->channel->dst_sec_func->partition, PARTITION_STATE_ACTIVE);

    if (offset >= active_msg->tx_size) {
        return 0;
    }

    uint32_t bytes_left = active_msg->tx_size - offset;
    len = (len > bytes_left) ? bytes_left : len;
    memcpy(buffer, active_msg->tx_buf + offset, len);

    return len;
}

void psa_write(psa_handle_t msg_handle, size_t offset, const void *buffer, size_t bytes)
{
    if (NULL == buffer) {
        spm_panic("%s - 'buffer' is NULL!\n", __func__);
    }

    active_msg_t *active_msg = NULL;
    spm_msg_handle_get_mem(msg_handle, &active_msg);

    SPM_ASSERT((active_msg->type > PSA_IPC_MSG_TYPE_INVALID) &&
               (active_msg->type <= PSA_IPC_MSG_TYPE_MAX));

    PARTITION_STATE_ASSERT(active_msg->channel->dst_sec_func->partition, PARTITION_STATE_ACTIVE);

    if (NULL == active_msg->rx_buf) {
        spm_panic("%s - active_msg->rx_buf is NULL!\n", __func__);
    }
    if (offset > UINT32_MAX - bytes) {
        spm_panic(
            "%s - Integer overflow (offset=%lu, bytes=%lu)!\n",
            __func__,
            offset,
            bytes
            );
    }
    if (offset + bytes > active_msg->rx_size) {
        spm_panic(
            "%s - Buffer overflow of client's response buffer"
            " (offset=%lu, bytes=%lu, active_msg->rx_size=%lu)!\n",
            __func__,
            offset,
            bytes,
            active_msg->rx_size
            );
    }

    memcpy((uint8_t *)(active_msg->rx_buf) + offset, buffer, bytes);
    return;
}

void psa_end(psa_handle_t msg_handle, psa_error_t retval)
{
    active_msg_t *active_msg = NULL;
    spm_msg_handle_get_mem(msg_handle, &active_msg);

    SPM_ASSERT((active_msg->type > PSA_IPC_MSG_TYPE_INVALID) &&
               (active_msg->type <= PSA_IPC_MSG_TYPE_MAX));

    if ((active_msg->channel->rhandle != NULL) && (active_msg->type == PSA_IPC_MSG_TYPE_DISCONNECT)){
        spm_panic("%s - Try closing channel without clearing rhandle first\n", __func__);
    }

    secure_func_t *dst_sec_func = active_msg->channel->dst_sec_func;

    PARTITION_STATE_ASSERT(dst_sec_func->partition, PARTITION_STATE_ACTIVE);

    active_msg->type = PSA_IPC_MSG_TYPE_INVALID; // Invalidate active_msg
    active_msg->rc = retval;

    dst_sec_func->partition->partition_state = PARTITION_STATE_COMPLETED;

    partition_t *curr_partition = active_partition_get();
    SPM_ASSERT(NULL != curr_partition);        // active thread in SPM must be in partition DB
    spm_msg_handle_destroy(curr_partition->msg_handle);
    curr_partition->msg_handle = PSA_NULL_HANDLE;
        
    int32_t flags = (int32_t)osThreadFlagsClear(dst_sec_func->mask);
    SPM_ASSERT(flags >= 0);
    PSA_UNUSED(flags);

    osStatus_t os_status = osSemaphoreRelease(dst_sec_func->partition->semaphore);
    SPM_ASSERT(osOK == os_status);
    PSA_UNUSED(os_status);

    return;
}

void psa_notify(int32_t partition_id)
{
    partition_t *target_partition = partition_get_by_pid(partition_id);
    SPM_ASSERT(NULL != target_partition);

    int32_t flags = (int32_t)osThreadFlagsSet(target_partition->thread_id, PSA_SIG_DOORBELL_MSK);
    SPM_ASSERT(flags >= 0);
    PSA_UNUSED(flags);
}

void psa_clear(void)
{
    int32_t flags = (int32_t)osThreadFlagsClear(PSA_SIG_DOORBELL_MSK);
    SPM_ASSERT(flags >= 0);
    PSA_UNUSED(flags);
}

int32_t psa_identity(psa_handle_t msg_handle)
{
    active_msg_t *active_msg = NULL;

    spm_msg_handle_get_mem(msg_handle, &active_msg);
    SPM_ASSERT(active_msg->channel != NULL);
    if (active_msg->channel->src_partition == NULL) {
        return PSA_NSPE_IDENTIFIER;
    }

    return active_msg->channel->src_partition->partition_id;
}

void psa_set_rhandle(psa_handle_t msg_handle, void *rhandle)
{
    active_msg_t *msg = NULL;
    spm_msg_handle_get_mem(msg_handle, &msg);
    msg->channel->rhandle = rhandle;
}