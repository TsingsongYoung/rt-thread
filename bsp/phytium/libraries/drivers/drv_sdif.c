/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Email: opensource_embedded@phytium.com.cn
 *
 * Change Logs:
 * Date        Author       Notes
 * 2023/7/11   liqiaozhong  init SD card and mount file system
 * 2023/11/8   zhugengyu    add interrupt handling for dma waiting, unify function naming
 */

/***************************** Include Files *********************************/
#include"rtconfig.h"

#ifdef BSP_USING_SDIF
#include <rthw.h>
#include <rtdef.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <rtdbg.h>
#include <drivers/mmcsd_core.h>

#ifdef RT_USING_SMART
    #include "ioremap.h"
#endif
#include "mm_aspace.h"
#include "interrupt.h"

#define LOG_TAG      "sdif_drv"
#include "drv_log.h"

#include "ftypes.h"
#include "fparameters.h"
#include "fcpu_info.h"

#include "fsdif_timing.h"

#include "fsdif.h"
#include "fsdif_hw.h"

#include "drv_sdif.h"
/************************** Constant Definitions *****************************/
#ifdef USING_SDIF0
    #define SDIF_CONTROLLER_ID    FSDIF0_ID
#elif defined (USING_SDIF1)
    #define SDIF_CONTROLLER_ID    FSDIF1_ID
#endif
#define SDIF_MALLOC_CAP_DESC  256U
#define SDIF_DMA_ALIGN        512U
#define SDIF_DMA_BLK_SZ       512U
#define SDIF_VALID_OCR        0x00FFFF80 /* supported voltage range is 1.65v-3.6v (VDD_165_195-VDD_35_36) */
#define SDIF_MAX_BLK_TRANS    20U

#ifndef CONFIG_SDCARD_OFFSET
    #define CONFIG_SDCARD_OFFSET 0x0U
#endif

/* preserve pointer to host instance */
static struct rt_mmcsd_host *mmc_host[FSDIF_NUM] = {RT_NULL};
/**************************** Type Definitions *******************************/
typedef struct
{
    FSdif *mmcsd_instance;
    FSdifIDmaDesc *rw_desc;
    rt_err_t (*transfer)(struct rt_mmcsd_host *host, struct rt_mmcsd_req *req, FSdifCmdData *cmd_data_p);
    struct rt_event event;
#define SDIF_EVENT_CARD_DETECTED    (1 << 0)
#define SDIF_EVENT_COMMAND_DONE     (1 << 1)
#define SDIF_EVENT_DATA_DONE        (1 << 2)
#define SDIF_EVENT_ERROR_OCCUR      (1 << 3)
#define SDIF_EVENT_SDIO_IRQ         (1 << 4)
} fsdif_info_t;
/************************** Variable Definitions *****************************/

/***************** Macros (Inline Functions) Definitions *********************/
void fsdif_change(void);

/*******************************Api Functions*********************************/
static void fsdif_host_relax(void)
{
    rt_thread_mdelay(1);
}

static void fsdif_card_detect_callback(FSdif *const mmcsd_instance, void *args, u32 status, u32 dmac_status)
{
    struct rt_mmcsd_host *host = (struct rt_mmcsd_host *)args;
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;

    rt_event_send(&private_data->event, SDIF_EVENT_CARD_DETECTED);
    fsdif_change();
}

static void fsdif_command_done_callback(FSdif *const mmcsd_instance, void *args, u32 status, u32 dmac_status)
{
    struct rt_mmcsd_host *host = (struct rt_mmcsd_host *)args;
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;

    rt_event_send(&private_data->event, SDIF_EVENT_COMMAND_DONE);
}

static void fsdif_data_done_callback(FSdif *const mmcsd_instance, void *args, u32 status, u32 dmac_status)
{
    struct rt_mmcsd_host *host = (struct rt_mmcsd_host *)args;
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;

    rt_event_send(&private_data->event, SDIF_EVENT_DATA_DONE);
}

static void fsdif_sdio_irq_callback(FSdif *const mmcsd_instance, void *args, u32 status, u32 dmac_status)
{
    struct rt_mmcsd_host *host = (struct rt_mmcsd_host *)args;
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;

    rt_event_send(&private_data->event, SDIF_EVENT_SDIO_IRQ);
}

static void fsdif_error_occur_callback(FSdif *const mmcsd_instance, void *args, u32 status, u32 dmac_status)
{
    struct rt_mmcsd_host *host = (struct rt_mmcsd_host *)args;
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;

    rt_event_send(&private_data->event, SDIF_EVENT_ERROR_OCCUR);
}

static void fsdif_ctrl_setup_interrupt(struct rt_mmcsd_host *host)
{
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;
    FSdif *mmcsd_instance = private_data->mmcsd_instance;
    FSdifConfig *config_p = &mmcsd_instance->config;
    rt_uint32_t cpu_id = 0;

    GetCpuId((u32 *)&cpu_id);
    rt_hw_interrupt_set_target_cpus(config_p->irq_num, cpu_id);
    rt_hw_interrupt_set_priority(config_p->irq_num, 0xd0);

    /* register intr callback */
    rt_hw_interrupt_install(config_p->irq_num,
                            FSdifInterruptHandler,
                            mmcsd_instance,
                            NULL);

    /* enable irq */
    rt_hw_interrupt_umask(config_p->irq_num);

    FSdifRegisterEvtHandler(mmcsd_instance, FSDIF_EVT_CARD_DETECTED, fsdif_card_detect_callback, host);
    FSdifRegisterEvtHandler(mmcsd_instance, FSDIF_EVT_ERR_OCCURE, fsdif_error_occur_callback, host);
    FSdifRegisterEvtHandler(mmcsd_instance, FSDIF_EVT_CMD_DONE, fsdif_command_done_callback, host);
    FSdifRegisterEvtHandler(mmcsd_instance, FSDIF_EVT_DATA_DONE, fsdif_data_done_callback, host);
    FSdifRegisterEvtHandler(mmcsd_instance, FSDIF_EVT_SDIO_IRQ, fsdif_sdio_irq_callback, host);

    return;
}

static rt_err_t fsdif_ctrl_init(struct rt_mmcsd_host *host)
{
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;
    FSdif *mmcsd_instance = RT_NULL;
    const FSdifConfig *default_mmcsd_config = RT_NULL;
    FSdifConfig mmcsd_config;
    FSdifIDmaDesc *rw_desc = RT_NULL;

    mmcsd_instance = rt_malloc(sizeof(FSdif));
    if (!mmcsd_instance)
    {
        LOG_E("Malloc mmcsd_instance failed");
        return RT_ERROR;
    }

    rw_desc = rt_malloc_align(SDIF_MAX_BLK_TRANS * sizeof(FSdifIDmaDesc), SDIF_MALLOC_CAP_DESC);
    if (!rw_desc)
    {
        LOG_E("Malloc rw_desc failed");
        return RT_ERROR;
    }

    rt_memset(mmcsd_instance, 0, sizeof(FSdif));
    rt_memset(rw_desc, 0, SDIF_MAX_BLK_TRANS * sizeof(FSdifIDmaDesc));

    /* SDIF controller init */
    RT_ASSERT((default_mmcsd_config = FSdifLookupConfig(SDIF_CONTROLLER_ID)) != RT_NULL);
    mmcsd_config = *default_mmcsd_config; /* load default config */
#ifdef RT_USING_SMART
    mmcsd_config.base_addr = (uintptr)rt_ioremap((void *)mmcsd_config.base_addr, 0x1000);
#endif
    mmcsd_config.trans_mode = FSDIF_IDMA_TRANS_MODE;
#ifdef USING_EMMC
    mmcsd_config.non_removable = TRUE; /* eMMC is unremovable on board */
#else
    mmcsd_config.non_removable = FALSE; /* TF card is removable on board */
#endif
    mmcsd_config.get_tuning = FSdifGetTimingSetting;

    if (FSDIF_SUCCESS != FSdifCfgInitialize(mmcsd_instance, &mmcsd_config))
    {
        LOG_E("SDIF controller init failed.");
        return RT_ERROR;
    }

    if (FSDIF_SUCCESS != FSdifSetIDMAList(mmcsd_instance, rw_desc, (uintptr)rw_desc + PV_OFFSET, SDIF_MAX_BLK_TRANS))
    {
        LOG_E("SDIF controller setup DMA failed.");
        return RT_ERROR;
    }
    mmcsd_instance->desc_list.first_desc_dma = (uintptr)rw_desc + PV_OFFSET;

    FSdifRegisterRelaxHandler(mmcsd_instance, fsdif_host_relax); /* SDIF delay for a while */

    private_data->mmcsd_instance = mmcsd_instance;
    private_data->rw_desc = rw_desc;

    fsdif_ctrl_setup_interrupt(host);
    return RT_EOK;
}

rt_inline rt_err_t fsdif_dma_transfer(struct rt_mmcsd_host *host, struct rt_mmcsd_req *req, FSdifCmdData *req_cmd)
{
    FError ret = FT_SUCCESS;
    rt_uint32_t event = 0U;
    rt_uint32_t wait_event = 0U;
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;
    FSdif *mmcsd_instance = private_data->mmcsd_instance;

    if (req_cmd->data_p == RT_NULL)
    {
        wait_event = SDIF_EVENT_COMMAND_DONE;
    }
    else
    {
        wait_event = SDIF_EVENT_COMMAND_DONE | SDIF_EVENT_DATA_DONE;
    }

    ret = FSdifDMATransfer(mmcsd_instance, req_cmd);
    if (ret != FT_SUCCESS)
    {
        LOG_E("FSdifDMATransfer() fail.");
        return -RT_ERROR;
    }

    while (TRUE)
    {
        if (rt_event_recv(&private_data->event,
                          (wait_event),
                          (RT_EVENT_FLAG_AND | RT_EVENT_FLAG_CLEAR | RT_WAITING_NO),
                          rt_tick_from_millisecond(5000),
                          &event) == RT_EOK)
        {
            (void)FSdifGetCmdResponse(mmcsd_instance, req_cmd);
            break;
        }
        else
        {
            if (rt_event_recv(&private_data->event,
                              (SDIF_EVENT_ERROR_OCCUR),
                              (RT_EVENT_FLAG_CLEAR | RT_WAITING_NO),
                              rt_tick_from_millisecond(5000),
                              &event) == RT_EOK)
            {
                LOG_E("Sdif DMA transfer endup with error !!!");
                return -RT_EIO;
            }
        }

        fsdif_host_relax();
    }

    if (resp_type(req->cmd) & RESP_MASK)
    {
        if (resp_type(req->cmd) == RESP_R2)
        {
            req->cmd->resp[3] = req_cmd->response[0];
            req->cmd->resp[2] = req_cmd->response[1];
            req->cmd->resp[1] = req_cmd->response[2];
            req->cmd->resp[0] = req_cmd->response[3];
        }
        else
        {
            req->cmd->resp[0] = req_cmd->response[0];
        }
    }

    return RT_EOK;
}

static void fsdif_request_send(struct rt_mmcsd_host *host, struct rt_mmcsd_req *req)
{
    /* ignore some SDIF-ONIY cmd */
    if ((req->cmd->cmd_code == SD_IO_SEND_OP_COND) || (req->cmd->cmd_code == SD_IO_RW_DIRECT))
    {
        req->cmd->err = -1;
        goto skip_cmd;
    }

    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;
    FSdifCmdData req_cmd;
    FSdifCmdData req_stop;
    FSdifData req_data;
    rt_uint32_t *data_buf_aligned = RT_NULL;
    rt_uint32_t cmd_flag = resp_type(req->cmd);

    rt_memset(&req_cmd, 0, sizeof(FSdifCmdData));
    rt_memset(&req_stop, 0, sizeof(FSdifCmdData));
    rt_memset(&req_data, 0, sizeof(FSdifData));

    /* convert req into ft driver type */
    if (req->cmd->cmd_code == GO_IDLE_STATE)
    {
        req_cmd.flag |= FSDIF_CMD_FLAG_NEED_INIT;
    }

    if (req->cmd->cmd_code == GO_INACTIVE_STATE)
    {
        req_cmd.flag |= FSDIF_CMD_FLAG_NEED_AUTO_STOP;
    }

    if ((cmd_flag != RESP_R3) && (cmd_flag != RESP_R4) && (cmd_flag != RESP_NONE))
    {
        req_cmd.flag |= FSDIF_CMD_FLAG_NEED_RESP_CRC;
    }

    if (cmd_flag & RESP_MASK)
    {
        req_cmd.flag |= FSDIF_CMD_FLAG_EXP_RESP;

        if (cmd_flag == RESP_R2)
        {
            req_cmd.flag |= FSDIF_CMD_FLAG_EXP_LONG_RESP;
        }
    }

    if (req->data) /* transfer command with data */
    {
        data_buf_aligned = rt_malloc_align(SDIF_DMA_BLK_SZ * req->data->blks, SDIF_DMA_ALIGN);
        if (!data_buf_aligned)
        {
            LOG_E("Malloc data_buf_aligned failed");
            return;
        }
        rt_memset(data_buf_aligned, 0, SDIF_DMA_BLK_SZ * req->data->blks);

        req_cmd.flag |= FSDIF_CMD_FLAG_EXP_DATA;

        req_data.blksz = req->data->blksize;
        req_data.blkcnt = req->data->blks + CONFIG_SDCARD_OFFSET;
        req_data.datalen = req->data->blksize * req->data->blks;
        if ((uintptr)req->data->buf % SDIF_DMA_ALIGN) /* data buffer should be 512-aligned */
        {
            if (req->data->flags & DATA_DIR_WRITE)
            {
                rt_memcpy((void *)data_buf_aligned, (void *)req->data->buf, req_data.datalen);
            }
            req_data.buf = (rt_uint8_t *)data_buf_aligned;
            req_data.buf_dma = (uintptr)data_buf_aligned + PV_OFFSET;
        }
        else
        {
            req_data.buf = (rt_uint8_t *)req->data->buf;
            req_data.buf_dma = (uintptr)req->data->buf + PV_OFFSET;
        }
        req_cmd.data_p = &req_data;

        if (req->data->flags & DATA_DIR_READ)
        {
            req_cmd.flag |= FSDIF_CMD_FLAG_READ_DATA;
        }
        else if (req->data->flags & DATA_DIR_WRITE)
        {
            req_cmd.flag |= FSDIF_CMD_FLAG_WRITE_DATA;
        }
    }

    req_cmd.cmdidx = req->cmd->cmd_code;
    req_cmd.cmdarg = req->cmd->arg;

    /* do cmd and data transfer */
    req->cmd->err = (private_data->transfer)(host, req, &req_cmd);
    if (req->cmd->err != RT_EOK)
    {
        LOG_E("transfer failed in %s", __func__);
    }

    if (req->data && (req->data->flags & DATA_DIR_READ))
    {
        if ((uintptr)req->data->buf % SDIF_DMA_ALIGN) /* data buffer should be 512-aligned */
        {
            rt_memcpy((void *)req->data->buf, (void *)data_buf_aligned, req_data.datalen);
        }
    }

    /* stop cmd */
    if (req->stop)
    {
        req_stop.cmdidx = req->stop->cmd_code;
        req_stop.cmdarg = req->stop->arg;
        if (req->stop->flags & RESP_MASK)
        {
            req_stop.flag |= FSDIF_CMD_FLAG_READ_DATA;
            if (resp_type(req->stop) == RESP_R2)
            {
                req_stop.flag |= FSDIF_CMD_FLAG_EXP_LONG_RESP;
            }
        }
        req->stop->err = (private_data->transfer)(host, req, &req_stop);
    }

    if (data_buf_aligned)
    {
        rt_free_align(data_buf_aligned);
    }

skip_cmd:
    mmcsd_req_complete(host);
}

static void fsdif_set_iocfg(struct rt_mmcsd_host *host, struct rt_mmcsd_io_cfg *io_cfg)
{
    FError ret = FT_SUCCESS;
    fsdif_info_t *private_data = (fsdif_info_t *)host->private_data;
    FSdif *mmcsd_instance = private_data->mmcsd_instance;
    uintptr base_addr = mmcsd_instance->config.base_addr;

    if (0 != io_cfg->clock)
    {
        ret = FSdifSetClkFreq(mmcsd_instance, io_cfg->clock);
        if (ret != FT_SUCCESS)
        {
            LOG_E("FSdifSetClkFreq fail.");
        }
    }

    switch (io_cfg->bus_width)
    {
        case MMCSD_BUS_WIDTH_1:
            FSdifSetBusWidth(base_addr, 1U);
            break;
        case MMCSD_BUS_WIDTH_4:
            FSdifSetBusWidth(base_addr, 4U);
            break;
        case MMCSD_BUS_WIDTH_8:
            FSdifSetBusWidth(base_addr, 8U);
            break;
        default:
            LOG_E("Invalid bus width %d", io_cfg->bus_width);
            break;
    }
}

static const struct rt_mmcsd_host_ops ops =
{
    fsdif_request_send,
    fsdif_set_iocfg,
    RT_NULL,
    RT_NULL,
    RT_NULL,
};

void fsdif_change(void)
{
    mmcsd_change(mmc_host[SDIF_CONTROLLER_ID]);
}

int rt_hw_fsdif_init(void)
{
    /* variables init */
    struct rt_mmcsd_host *host = RT_NULL;
    fsdif_info_t *private_data = RT_NULL;
    rt_err_t result = RT_EOK;

    host = mmcsd_alloc_host();
    if (!host)
    {
        LOG_E("Alloc host failed");
        goto err_free;
    }

    private_data = rt_malloc(sizeof(fsdif_info_t));
    if (!private_data)
    {
        LOG_E("Malloc private_data failed");
        goto err_free;
    }

    rt_memset(private_data, 0, sizeof(fsdif_info_t));
    private_data->transfer = fsdif_dma_transfer;
    result = rt_event_init(&private_data->event, "sdif_event", RT_IPC_FLAG_FIFO);
    RT_ASSERT(RT_EOK == result);

    /* host data init */
    host->ops = &ops;
    host->freq_min = 400000;
    host->freq_max = 50000000;
    host->valid_ocr = SDIF_VALID_OCR; /* the voltage range supported is 1.65v-3.6v */
    host->flags = MMCSD_MUTBLKWRITE | MMCSD_BUSWIDTH_4;
    host->max_seg_size = SDIF_DMA_BLK_SZ; /* used in block_dev.c */
    host->max_dma_segs = SDIF_MAX_BLK_TRANS; /* physical segment number */
    host->max_blk_size = SDIF_DMA_BLK_SZ; /* all the 4 para limits size of one blk tran */
    host->max_blk_count = SDIF_MAX_BLK_TRANS;
    host->private_data = private_data;

    mmc_host[SDIF_CONTROLLER_ID] = host;

    if (RT_EOK != fsdif_ctrl_init(host))
    {
        LOG_E("fsdif_ctrl_init() failed");
        goto err_free;
    }

    return RT_EOK;

err_free:
    if (host)
    {
        rt_free(host);
    }
    if (private_data->mmcsd_instance)
    {
        rt_free(private_data->mmcsd_instance);
    }
    if (private_data->rw_desc)
    {
        rt_free_align(private_data->rw_desc);
    }
    if (private_data)
    {
        rt_free(private_data);
    }

    return -RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_fsdif_init);
#endif // #ifdef RT_USING_SDIO