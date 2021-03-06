/*
 * Copyright 2015 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(_WIN32)
#include "vld.h"
#endif

#define MODULE_TAG "mpi_multi_test"

#include <string.h>
#include "rk_mpi.h"

#include "mpp_log.h"
#include "mpp_mem.h"
#include "mpp_env.h"
#include "mpp_time.h"
#include "mpp_common.h"
#include "vpu_api.h"

#include "utils.h"

#include <pthread.h>

#define MPI_RESOLUTION_COUNT            10
#define MPI_DEC_LOOP_COUNT                  4
#define MPI_DEC_STREAM_SIZE                 (SZ_4K)
#define MPI_ENC_IO_COUNT                    (4)
#define MAX_FILE_NAME_LENGTH                256

#define MPI_ENC_TEST_SET_IDR_FRAME  0
#define MPI_ENC_TEST_SET_OSD            0
#define MPI_ENC_TEST_SET_ROI            1

typedef struct {
    MppCtx          ctx;
    MppApi          *mpi;

    /* end of stream flag when set quit the loop */
    RK_U32          eos;

    /* buffer for stream data reading */
    char            *buf;

    /* input and output */
    MppBufferGroup  frm_grp;
    MppBufferGroup  pkt_grp;
    MppPacket       packet;
    size_t          packet_size;
    MppFrame        frame;

    FILE            *fp_input;
    FILE            *fp_output;
    RK_U32          frame_count;
} MpiDecLoopData;

typedef struct {
    // global flow control flag
    RK_U32 frm_eos;
    RK_U32 pkt_eos;
    RK_U32 frame_count;
    RK_U64 stream_size;
    // src and dst
    FILE *fp_input;
    FILE *fp_output;
    // base flow context
    MppCtx ctx;
    MppApi *mpi;
    MppEncPrepCfg prep_cfg;
    MppEncRcCfg rc_cfg;
    MppEncCodecCfg codec_cfg;
    // input / output
    MppBufferGroup frm_grp;
    MppBufferGroup pkt_grp;
    MppFrame  frame;
    MppPacket packet;
    MppBuffer frm_buf[MPI_ENC_IO_COUNT];
    MppBuffer pkt_buf[MPI_ENC_IO_COUNT];
    MppBuffer md_buf[MPI_ENC_IO_COUNT];
    MppBuffer osd_idx_buf[MPI_ENC_IO_COUNT];
    MppEncOSDPlt osd_plt;
    MppEncROIRegion roi_region[3]; /* can be more regions */
    MppEncSeiMode sei_mode;
    // paramter for resource malloc
    RK_U32 width;
    RK_U32 height;
    RK_U32 hor_stride;
    RK_U32 ver_stride;
    MppFrameFormat fmt;
    MppCodingType type;
    RK_U32 num_frames;
    // resources
    size_t frame_size;    /* NOTE: packet buffer may overflow */
    size_t packet_size;    /* 32bits for each 16x16 block */
    size_t mdinfo_size;    /* osd idx size range from 16x16 bytes(pixels) to hor_stride*ver_stride(bytes). for general use, 1/8 Y buffer is enough. */
    size_t osd_idx_size;
    RK_U32 plt_table[8];    // rate control runtime parameter
    RK_S32 gop;
    RK_S32 fps;
    RK_S32 bps;
    RK_S32 qp_min;
    RK_S32 qp_max;
    RK_S32 qp_step;
    RK_S32 qp_init;
} MpiEncTestData;

typedef struct {
    char            file_input[MAX_FILE_NAME_LENGTH];
    char            file_output[MAX_FILE_NAME_LENGTH];
    MppCodingType   type;
    RK_U32          width;
    RK_U32          height;
    MppFrameFormat  format;
    RK_U32          debug;
    RK_U32          num_frames;

    RK_U32          have_input;
    RK_U32          have_output;

    RK_U32          simple;
    RK_S32          timeout;

    RK_U32          nthreads;
    MppEncRcMode    rc_mode;// 0:VBR, 1:CBR
    RK_U32      nTestType;// 1:decode, 2:encode
} MpiTestCmd;

typedef struct {
    MpiTestCmd      cmd[MPI_RESOLUTION_COUNT];
    RK_U32          nResoultions;
} MpiTestParam;

static OptionInfo mpi_multi_cmd[] = {
    {"i",               "input_file",           "input bitstream file"},
    {"o",               "output_file",          "output bitstream file, "},
    {"w",               "width",                "the width of input bitstream"},
    {"h",               "height",               "the height of input bitstream"},
    {"t",               "type",                 "input stream coding type"},
    {"d",              "debug",                "debug flag"},
    {"x",               "timeout",              "output timeout interval"},
    {"n",              "max frame number",     "max encoding frame number"},
    {"f",               "format",               "the format of input picture"},
    {"q",              "test_type",           "mpp test type"},
};

static MPP_RET mpi_enc_gen_osd_data(MppEncOSDData *osd_data, MppBuffer osd_buf, RK_U32 frame_cnt)
{
    RK_U32 k = 0, buf_size = 0;
    RK_U8 data = 0;

    osd_data->num_region = 8;
    osd_data->buf = osd_buf;
    for (k = 0; k < osd_data->num_region; k++) {
        osd_data->region[k].enable = 1;
        osd_data->region[k].inverse = frame_cnt & 1;
        osd_data->region[k].start_mb_x = k * 3;
        osd_data->region[k].start_mb_y = k * 2;
        osd_data->region[k].num_mb_x = 2;
        osd_data->region[k].num_mb_y = 2;

        buf_size = osd_data->region[k].num_mb_x * osd_data->region[k].num_mb_y * 256;
        osd_data->region[k].buf_offset = k * buf_size;

        data = k;
        memset((RK_U8 *)mpp_buffer_get_ptr(osd_data->buf) + osd_data->region[k].buf_offset, data, buf_size);
    }

    return MPP_OK;
}

static MPP_RET mpi_enc_gen_osd_plt(MppEncOSDPlt *osd_plt, RK_U32 *table)
{
    RK_U32 k = 0;
    if (osd_plt->buf) {
        for (k = 0; k < 256; k++)
            osd_plt->buf[k] = table[k % 8];
    }
    return MPP_OK;
}

MPP_RET test_ctx_init(MpiEncTestData **data, MpiTestCmd *cmd)
{
    MpiEncTestData *p = NULL;
    MPP_RET ret = MPP_OK;

    if (!data || !cmd) {
        mpp_err_f("invalid input data %p cmd %p\n", data, cmd);
        return MPP_ERR_NULL_PTR;
    }

    p = mpp_calloc(MpiEncTestData, 1);
    if (!p) {
        mpp_err_f("create MpiEncTestData failed\n");
        ret = MPP_ERR_MALLOC;
        goto RET;
    }

    // get paramter from cmd
    p->width        = cmd->width;
    p->height       = cmd->height;
    p->hor_stride   = MPP_ALIGN(cmd->width, 16);
    p->ver_stride   = MPP_ALIGN(cmd->height, 16);
    p->fmt          = cmd->format;
    p->type         = cmd->type;
    p->num_frames   = cmd->num_frames;
    p->rc_cfg.rc_mode = cmd->rc_mode;
    mpp_log("rc_mode=%d\n", p->rc_cfg.rc_mode);

    if (cmd->have_input) {
        p->fp_input = fopen(cmd->file_input, "rb");
        if (NULL == p->fp_input) {
            mpp_err("failed to open input file %s\n", cmd->file_input);
            mpp_err("create default yuv image for test\n");
        }
    }

    if (cmd->have_output) {
        char file_name[MAX_FILE_NAME_LENGTH];
        int pthread_id = pthread_self();
        sprintf(file_name, "%s%d", cmd->file_output, pthread_id);
        mpp_log("===================file_name=%s\n", file_name);

        p->fp_output = fopen(file_name, "w+b");
        if (NULL == p->fp_output) {
            mpp_err("failed to open output file %s\n", file_name);
            ret = MPP_ERR_OPEN_FILE;
        }
    }

    // update resource parameter
    if (p->fmt <= MPP_FMT_YUV_BUTT)
        p->frame_size = p->hor_stride * p->ver_stride * 3 / 2;
    else
        p->frame_size = p->hor_stride * p->ver_stride * 4;
    p->packet_size  = p->width * p->height;
    //NOTE: hor_stride should be 16-MB aligned
    p->mdinfo_size  = (((p->hor_stride + 255) & (~255)) / 16) * (p->ver_stride / 16) * 4;
    /*
     * osd idx size range from 16x16 bytes(pixels) to hor_stride*ver_stride(bytes).
     * for general use, 1/8 Y buffer is enough.
     */
    p->osd_idx_size  = p->hor_stride * p->ver_stride / 8;
    p->plt_table[0] = MPP_ENC_OSD_PLT_WHITE;
    p->plt_table[1] = MPP_ENC_OSD_PLT_YELLOW;
    p->plt_table[2] = MPP_ENC_OSD_PLT_CYAN;
    p->plt_table[3] = MPP_ENC_OSD_PLT_GREEN;
    p->plt_table[4] = MPP_ENC_OSD_PLT_TRANS;
    p->plt_table[5] = MPP_ENC_OSD_PLT_RED;
    p->plt_table[6] = MPP_ENC_OSD_PLT_BLUE;
    p->plt_table[7] = MPP_ENC_OSD_PLT_BLACK;

RET:
    *data = p;
    return ret;
}

MPP_RET test_ctx_deinit(MpiEncTestData **data)
{
    MpiEncTestData *p = NULL;

    if (!data) {
        mpp_err_f("invalid input data %p\n", data);
        return MPP_ERR_NULL_PTR;
    }

    p = *data;
    if (p) {
        if (p->fp_input) {
            fclose(p->fp_input);
            p->fp_input = NULL;
        }
        if (p->fp_output) {
            fclose(p->fp_output);
            p->fp_output = NULL;
        }
        MPP_FREE(p);
        *data = NULL;
    }

    return MPP_OK;
}

MPP_RET test_res_init(MpiEncTestData *p)
{
    RK_U32 i;
    MPP_RET ret;

    mpp_assert(p);

    ret = mpp_buffer_group_get_internal(&p->frm_grp, MPP_BUFFER_TYPE_ION);
    if (ret) {
        mpp_err("failed to get buffer group for input frame ret %d\n", ret);
        goto RET;
    }

    ret = mpp_buffer_group_get_internal(&p->pkt_grp, MPP_BUFFER_TYPE_ION);
    if (ret) {
        mpp_err("failed to get buffer group for output packet ret %d\n", ret);
        goto RET;
    }

    for (i = 0; i < MPI_ENC_IO_COUNT; i++) {
        ret = mpp_buffer_get(p->frm_grp, &p->frm_buf[i], p->frame_size);
        if (ret) {
            mpp_err("failed to get buffer for input frame ret %d\n", ret);
            goto RET;
        }

        ret = mpp_buffer_get(p->frm_grp, &p->osd_idx_buf[i], p->osd_idx_size);
        if (ret) {
            mpp_err("failed to get buffer for osd idx buf ret %d\n", ret);
            goto RET;
        }

        ret = mpp_buffer_get(p->pkt_grp, &p->pkt_buf[i], p->packet_size);
        if (ret) {
            mpp_err("failed to get buffer for input frame ret %d\n", ret);
            goto RET;
        }

        ret = mpp_buffer_get(p->pkt_grp, &p->md_buf[i], p->mdinfo_size);
        if (ret) {
            mpp_err("failed to get buffer for motion detection info ret %d\n", ret);
            goto RET;
        }
    }
RET:
    return ret;
}

MPP_RET test_res_deinit(MpiEncTestData *p)
{
    RK_U32 i;

    mpp_assert(p);

    for (i = 0; i < MPI_ENC_IO_COUNT; i++) {
        if (p->frm_buf[i]) {
            mpp_buffer_put(p->frm_buf[i]);
            p->frm_buf[i] = NULL;
        }

        if (p->pkt_buf[i]) {
            mpp_buffer_put(p->pkt_buf[i]);
            p->pkt_buf[i] = NULL;
        }

        if (p->md_buf[i]) {
            mpp_buffer_put(p->md_buf[i]);
            p->md_buf[i] = NULL;
        }

        if (p->osd_idx_buf[i]) {
            mpp_buffer_put(p->osd_idx_buf[i]);
            p->osd_idx_buf[i] = NULL;
        }
    }

    if (p->frm_grp) {
        mpp_buffer_group_put(p->frm_grp);
        p->frm_grp = NULL;
    }

    if (p->pkt_grp) {
        mpp_buffer_group_put(p->pkt_grp);
        p->pkt_grp = NULL;
    }

    return MPP_OK;
}

MPP_RET test_mpp_init(MpiEncTestData *p)
{
    MPP_RET ret;

    if (NULL == p)
        return MPP_ERR_NULL_PTR;

    ret = mpp_create(&p->ctx, &p->mpi);
    if (ret) {
        mpp_err("mpp_create failed ret %d\n", ret);
        goto RET;
    }

    ret = mpp_init(p->ctx, MPP_CTX_ENC, p->type);
    if (ret)
        mpp_err("mpp_init failed ret %d\n", ret);

RET:
    return ret;
}

MPP_RET test_mpp_setup(MpiEncTestData *p)
{
    MPP_RET ret;
    MppApi *mpi;
    MppCtx ctx;
    MppEncCodecCfg *codec_cfg;
    MppEncPrepCfg *prep_cfg;
    MppEncRcCfg *rc_cfg;

    if (NULL == p)
        return MPP_ERR_NULL_PTR;

    mpi = p->mpi;
    ctx = p->ctx;
    codec_cfg = &p->codec_cfg;
    prep_cfg = &p->prep_cfg;
    rc_cfg = &p->rc_cfg;

    /* setup default parameter */
    p->fps = 30;
    p->gop = 60;
    p->bps = p->width * p->height / 8 * p->fps;
    p->qp_init  = (p->type == MPP_VIDEO_CodingMJPEG) ? (10) : (26);

    prep_cfg->change        = MPP_ENC_PREP_CFG_CHANGE_INPUT |
                              MPP_ENC_PREP_CFG_CHANGE_ROTATION |
                              MPP_ENC_PREP_CFG_CHANGE_FORMAT;
    prep_cfg->width         = p->width;
    prep_cfg->height        = p->height;
    prep_cfg->hor_stride    = p->hor_stride;
    prep_cfg->ver_stride    = p->ver_stride;
    prep_cfg->format        = p->fmt;
    prep_cfg->rotation      = MPP_ENC_ROT_0;
    ret = mpi->control(ctx, MPP_ENC_SET_PREP_CFG, prep_cfg);
    if (ret) {
        mpp_err("mpi control enc set prep cfg failed ret %d\n", ret);
        goto RET;
    }

    rc_cfg->change  = MPP_ENC_RC_CFG_CHANGE_ALL;
    rc_cfg->rc_mode = p->rc_cfg.rc_mode;
    rc_cfg->quality = MPP_ENC_RC_QUALITY_MEDIUM;

    if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_CBR) {
        /* constant bitrate has very small bps range of 1/16 bps */
        rc_cfg->bps_target   = p->bps;
        rc_cfg->bps_max      = p->bps * 17 / 16;
        rc_cfg->bps_min      = p->bps * 15 / 16;
    } else if (rc_cfg->rc_mode ==  MPP_ENC_RC_MODE_VBR) {
        if (rc_cfg->quality == MPP_ENC_RC_QUALITY_CQP) {
            /* constant QP does not have bps */
            rc_cfg->bps_target   = -1;
            rc_cfg->bps_max      = -1;
            rc_cfg->bps_min      = -1;
        } else {
            /* variable bitrate has large bps range */
            rc_cfg->bps_target   = p->bps;
            rc_cfg->bps_max      = p->bps * 17 / 16;
            rc_cfg->bps_min      = p->bps * 1 / 16;
        }
    }

    /* fix input / output frame rate */
    rc_cfg->fps_in_flex      = 0;
    rc_cfg->fps_in_num       = p->fps;
    rc_cfg->fps_in_denorm    = 1;
    rc_cfg->fps_out_flex     = 0;
    rc_cfg->fps_out_num      = p->fps;
    rc_cfg->fps_out_denorm   = 1;

    rc_cfg->gop              = p->gop;
    rc_cfg->skip_cnt         = 0;

    mpp_log("mpi_enc_test bps %d fps %d gop %d\n",
            rc_cfg->bps_target, rc_cfg->fps_out_num, rc_cfg->gop);
    ret = mpi->control(ctx, MPP_ENC_SET_RC_CFG, rc_cfg);
    if (ret) {
        mpp_err("mpi control enc set rc cfg failed ret %d\n", ret);
        goto RET;
    }

    codec_cfg->coding = p->type;
    switch (codec_cfg->coding) {
    case MPP_VIDEO_CodingAVC : {
        codec_cfg->h264.change = MPP_ENC_H264_CFG_CHANGE_PROFILE |
                                 MPP_ENC_H264_CFG_CHANGE_ENTROPY |
                                 MPP_ENC_H264_CFG_CHANGE_TRANS_8x8 |
                                 MPP_ENC_H264_CFG_CHANGE_QP_LIMIT;
        /*
         * H.264 profile_idc parameter
         * 66  - Baseline profile
         * 77  - Main profile
         * 100 - High profile
         */
        codec_cfg->h264.profile  = 100;
        /*
         * H.264 level_idc parameter
         * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
         * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
         * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
         * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
         * 50 / 51 / 52         - 4K@30fps
         */
        codec_cfg->h264.level    = 40;
        codec_cfg->h264.entropy_coding_mode  = 1;
        codec_cfg->h264.cabac_init_idc  = 0;
        codec_cfg->h264.transform8x8_mode = 1;

        if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_CBR) {
            /* constant bitrate do not limit qp range */
            p->qp_max   = 48;
            p->qp_min   = 4;
            p->qp_step  = 16;
            p->qp_init  = 0;
        } else if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_VBR) {
            if (rc_cfg->quality == MPP_ENC_RC_QUALITY_CQP) {
                /* constant QP mode qp is fixed */
                p->qp_max   = p->qp_init;
                p->qp_min   = p->qp_init;
                p->qp_step  = 0;
            } else {
                /* variable bitrate has qp min limit */
                p->qp_max   = 40;
                p->qp_min   = 12;
                p->qp_step  = 8;
                p->qp_init  = 0;
            }
        }

        codec_cfg->h264.qp_max      = p->qp_max;
        codec_cfg->h264.qp_min      = p->qp_min;
        codec_cfg->h264.qp_max_step = p->qp_step;
        codec_cfg->h264.qp_init     = p->qp_init;
    } break;
    case MPP_VIDEO_CodingMJPEG : {
        codec_cfg->jpeg.change  = MPP_ENC_JPEG_CFG_CHANGE_QP;
        codec_cfg->jpeg.quant   = p->qp_init;
    } break;
    case MPP_VIDEO_CodingVP8 :
    case MPP_VIDEO_CodingHEVC :
    default : {
        mpp_err_f("support encoder coding type %d\n", codec_cfg->coding);
    } break;
    }
    ret = mpi->control(ctx, MPP_ENC_SET_CODEC_CFG, codec_cfg);
    if (ret) {
        mpp_err("mpi control enc set codec cfg failed ret %d\n", ret);
        goto RET;
    }

    /* optional */
    p->sei_mode = MPP_ENC_SEI_MODE_ONE_FRAME;
    ret = mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &p->sei_mode);
    if (ret) {
        mpp_err("mpi control enc set sei cfg failed ret %d\n", ret);
        goto RET;
    }

    /* gen and cfg osd plt */
    mpi_enc_gen_osd_plt(&p->osd_plt, p->plt_table);
#if MPI_ENC_TEST_SET_OSD
    ret = mpi->control(ctx, MPP_ENC_SET_OSD_PLT_CFG, &p->osd_plt);
    if (ret) {
        mpp_err("mpi control enc set osd plt failed ret %d\n", ret);
        goto RET;
    }
#endif

RET:
    return ret;
}

/*
 * write header here
 */
MPP_RET test_mpp_preprare(MpiEncTestData *p)
{
    MPP_RET ret;
    MppApi *mpi;
    MppCtx ctx;
    MppPacket packet = NULL;

    if (NULL == p)
        return MPP_ERR_NULL_PTR;

    mpi = p->mpi;
    ctx = p->ctx;
    ret = mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &packet);
    if (ret) {
        mpp_err("mpi control enc get extra info failed\n");
        goto RET;
    }

    /* get and write sps/pps for H.264 */
    if (packet) {
        void *ptr   = mpp_packet_get_pos(packet);
        size_t len  = mpp_packet_get_length(packet);

        if (p->fp_output)
            fwrite(ptr, 1, len, p->fp_output);

        packet = NULL;
    }
RET:
    return ret;
}

MPP_RET test_mpp_run(MpiEncTestData *p)
{
    MPP_RET ret;
    MppApi *mpi;
    MppCtx ctx;
    MppPacket packet = NULL;
    RK_S32 i;
    RK_S64 p_s, p_e, diff;

    if (NULL == p)
        return MPP_ERR_NULL_PTR;

    mpi = p->mpi;
    ctx = p->ctx;

    p_s = mpp_time();
    ret = mpp_frame_init(&p->frame);
    if (ret) {
        mpp_err_f("mpp_frame_init failed\n");
        goto RET;
    }

    mpp_frame_set_width(p->frame, p->width);
    mpp_frame_set_height(p->frame, p->height);
    mpp_frame_set_hor_stride(p->frame, p->hor_stride);
    mpp_frame_set_ver_stride(p->frame, p->ver_stride);
    mpp_frame_set_fmt(p->frame, p->fmt);

    i = 0;
    while (!p->pkt_eos) {
        MppTask task = NULL;
        RK_S32 index = i++;
        MppBuffer frm_buf_in  = p->frm_buf[index];
        MppBuffer pkt_buf_out = p->pkt_buf[index];
        MppBuffer md_info_buf = p->md_buf[index];
        MppBuffer osd_data_buf = p->osd_idx_buf[index];
        MppEncOSDData osd_data;
        void *buf = mpp_buffer_get_ptr(frm_buf_in);

        if (i == MPI_ENC_IO_COUNT)
            i = 0;

        if (p->fp_input) {
            ret = read_yuv_image(buf, p->fp_input, p->width, p->height,
                                 p->hor_stride, p->ver_stride, p->fmt);
            if (ret == MPP_NOK  || feof(p->fp_input)) {
                mpp_log("found last frame. feof %d\n", feof(p->fp_input));
                p->frm_eos = 1;
            } else if (ret == MPP_ERR_VALUE)
                goto RET;
        } else {
            ret = fill_yuv_image(buf, p->width, p->height, p->hor_stride,
                                 p->ver_stride, p->fmt, p->frame_count);
            if (ret)
                goto RET;
        }

        mpp_frame_set_buffer(p->frame, frm_buf_in);
        mpp_frame_set_eos(p->frame, p->frm_eos);

        mpp_packet_init_with_buffer(&packet, pkt_buf_out);

        ret = mpi->poll(ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
        if (ret) {
            mpp_err("mpp task input poll failed ret %d\n", ret);
            goto RET;
        }

        ret = mpi->dequeue(ctx, MPP_PORT_INPUT, &task);
        if (ret || NULL == task) {
            mpp_err("mpp task input dequeue failed ret %d task %p\n", ret, task);
            goto RET;
        }

        mpp_task_meta_set_frame (task, KEY_INPUT_FRAME,  p->frame);
        mpp_task_meta_set_packet(task, KEY_OUTPUT_PACKET, packet);
        mpp_task_meta_set_buffer(task, KEY_MOTION_INFO, md_info_buf);

        /* set idr frame */
#if MPI_ENC_TEST_SET_IDR_FRAME
        if (p->frame_count && p->frame_count % (p->gop / 4) == 0) {
            ret = mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, NULL);
            if (MPP_OK != ret) {
                mpp_err("mpi control enc set idr frame failed\n");
                goto RET;
            }
        }
#endif

        /* gen and cfg osd plt */
        mpi_enc_gen_osd_data(&osd_data, osd_data_buf, p->frame_count);
#if MPI_ENC_TEST_SET_OSD
        ret = mpi->control(ctx, MPP_ENC_SET_OSD_DATA_CFG, &osd_data);
        if (MPP_OK != ret) {
            mpp_err("mpi control enc set osd data failed\n");
            goto RET;
        }
#endif

#if MPI_ENC_TEST_SET_ROI
        if (p->type == MPP_VIDEO_CodingAVC) {
            MppEncROIRegion *region = p->roi_region;
            MppEncROICfg roi_cfg;

            /* calculated in pixels */
            region->x = region->y = 64;
            region->w = region->h = 128; /* 16-pixel aligned is better */
            region->intra = 0;   /* flag of forced intra macroblock */
            region->quality = 20; /* qp of macroblock */

            region++;
            region->x = region->y = 256;
            region->w = region->h = 128; /* 16-pixel aligned is better */
            region->intra = 1;   /* flag of forced intra macroblock */
            region->quality = 25; /* qp of macroblock */

            roi_cfg.number = 2;
            roi_cfg.regions = p->roi_region;

            ret = mpi->control(ctx, MPP_ENC_SET_ROI_CFG, &roi_cfg);
            if (MPP_OK != ret) {
                mpp_err("mpi control enc set roi data failed\n");
                goto RET;
            }
        }
#endif

        ret = mpi->enqueue(ctx, MPP_PORT_INPUT, task);
        if (ret) {
            mpp_err("mpp task input enqueue failed\n");
            goto RET;
        }

        ret = mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
        if (ret) {
            mpp_err("mpp task output poll failed ret %d\n", ret);
            goto RET;
        }

        ret = mpi->dequeue(ctx, MPP_PORT_OUTPUT, &task);
        if (ret || NULL == task) {
            mpp_err("mpp task output dequeue failed ret %d task %p\n", ret, task);
            goto RET;
        }

        if (task) {
            MppFrame packet_out = NULL;

            mpp_task_meta_get_packet(task, KEY_OUTPUT_PACKET, &packet_out);

            mpp_assert(packet_out == packet);
            if (packet) {
                // write packet to file here
                void *ptr   = mpp_packet_get_pos(packet);
                size_t len  = mpp_packet_get_length(packet);

                p->pkt_eos = mpp_packet_get_eos(packet);

                if (p->fp_output)
                    fwrite(ptr, 1, len, p->fp_output);
                mpp_packet_deinit(&packet);

                mpp_log_f("encoded frame %d size %d\n", p->frame_count, len);
                p->stream_size += len;

                if (p->pkt_eos) {
                    mpp_log("found last packet\n");
                    mpp_assert(p->frm_eos);
                }
            }
            p->frame_count++;
        }

        ret = mpi->enqueue(ctx, MPP_PORT_OUTPUT, task);
        if (ret) {
            mpp_err("mpp task output enqueue failed\n");
            goto RET;
        }

        if (p->num_frames && p->frame_count >= p->num_frames) {
            mpp_log_f("encode max %d frames", p->frame_count);
            break;
        }
        if (p->frm_eos && p->pkt_eos)
            break;
    }

    p_e = mpp_time();
    diff = (p_e - p_s) / 1000;
    mpp_log("chn encode %d frames use time %lld frm_rate:%d.\n",
            p->frame_count, diff, p->frame_count * 1000 / diff);

RET:
    if (p->frame) {
        mpp_frame_deinit(&p->frame);
        p->frame = NULL;
    }

    return ret;
}

MPP_RET test_mpp_deinit(MpiEncTestData *p)
{
    if (p->ctx) {
        mpp_destroy(p->ctx);
        p->ctx = NULL;
    }

    return MPP_OK;
}

void* mpi_enc_test(void *cmd_ctx)
{
    MPP_RET ret = MPP_OK;
    MpiEncTestData *p = NULL;
    MpiTestCmd *cmd = (MpiTestCmd *)cmd_ctx;
    RK_S64 t_s, t_e, t_diff = 1;

    ret = test_ctx_init(&p, cmd);
    if (ret) {
        mpp_err_f("test data init failed ret %d\n", ret);
        goto MPP_TEST_OUT;
    }

    ret = test_res_init(p);
    if (ret) {
        mpp_err_f("test resource init failed ret %d\n", ret);
        goto MPP_TEST_OUT;
    }

    mpp_log("mpi_enc_test encoder test start w %d h %d type %d\n",
            p->width, p->height, p->type);

    printf("==>ENC [%d x %d] test start\n", p->width, p->height);

    // encoder demo
    ret = test_mpp_init(p);
    if (ret) {
        mpp_err_f("test mpp init failed ret %d\n", ret);
        goto MPP_TEST_OUT;
    }

    ret = test_mpp_setup(p);
    if (ret) {
        mpp_err_f("test mpp setup failed ret %d\n", ret);
        goto MPP_TEST_OUT;
    }

    ret = test_mpp_preprare(p);
    if (ret) {
        mpp_err_f("test mpp prepare failed ret %d\n", ret);
        goto MPP_TEST_OUT;
    }

    t_s = mpp_time();
    ret = test_mpp_run(p);
    if (ret) {
        mpp_err_f("test mpp run failed ret %d\n", ret);
        goto MPP_TEST_OUT;
    }

    ret = p->mpi->reset(p->ctx);
    if (ret) {
        mpp_err("mpi->reset failed\n");
        goto MPP_TEST_OUT;
    }
    t_e = mpp_time();
    t_diff = (t_e - t_s) / 1000;

MPP_TEST_OUT:
    test_mpp_deinit(p);

    test_res_deinit(p);

    if (MPP_OK == ret)
        mpp_log("mpi_enc_test success total frame %d bps %lld\n",
                p->frame_count, (RK_U64)((p->stream_size * 8 * p->fps) / p->frame_count));
    else
        mpp_err("mpi_enc_test failed ret %d\n", ret);

    RK_U32 *rate = malloc(sizeof(RK_U32));
    mpp_assert(rate != NULL);
    *rate = (p->frame_count * 1000) / t_diff;

    printf("==>ENC [%d x %d]  rates=%d\n", p->width, p->height, *rate);
    test_ctx_deinit(&p);

    return rate;
}

static int decode_simple(MpiDecLoopData *data)
{
    RK_U32 pkt_done = 0;
    RK_U32 pkt_eos  = 0;
    RK_U32 err_info = 0;
    MPP_RET ret = MPP_OK;
    MppCtx ctx  = data->ctx;
    MppApi *mpi = data->mpi;
    char   *buf = data->buf;
    MppPacket packet = data->packet;
    MppFrame  frame  = NULL;
    size_t read_size = fread(buf, 1, data->packet_size, data->fp_input);

    if (read_size != data->packet_size || feof(data->fp_input)) {
        mpp_log("found last packet\n");

        // setup eos flag
        data->eos = pkt_eos = 1;
    }

    // write data to packet
    mpp_packet_write(packet, 0, buf, read_size);
    // reset pos and set valid length
    mpp_packet_set_pos(packet, buf);
    mpp_packet_set_length(packet, read_size);
    // setup eos flag
    if (pkt_eos)
        mpp_packet_set_eos(packet);

    do {
        RK_S32 times = 5;
        // send the packet first if packet is not done
        if (!pkt_done) {
            ret = mpi->decode_put_packet(ctx, packet);
            if (MPP_OK == ret)
                pkt_done = 1;
        }

        // then get all available frame and release
        do {
            RK_S32 get_frm = 0;
            RK_U32 frm_eos = 0;

        try_again:
            ret = mpi->decode_get_frame(ctx, &frame);
            if (MPP_ERR_TIMEOUT == ret) {
                if (times > 0) {
                    times--;
                    msleep(2);
                    goto try_again;
                }
                mpp_err("decode_get_frame failed too much time\n");
            }
            if (MPP_OK != ret) {
                mpp_err("decode_get_frame failed ret %d\n", ret);
                break;
            }

            if (frame) {
                if (mpp_frame_get_info_change(frame)) {
                    RK_U32 width = mpp_frame_get_width(frame);
                    RK_U32 height = mpp_frame_get_height(frame);
                    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);

                    mpp_log("decode_get_frame get info changed found\n");
                    mpp_log("decoder require buffer w:h [%d:%d] stride [%d:%d]",
                            width, height, hor_stride, ver_stride);

                    /*
                     * NOTE: We can choose decoder's buffer mode here.
                     * There are three mode that decoder can support:
                     *
                     * Mode 1: Pure internal mode
                     * In the mode user will NOT call MPP_DEC_SET_EXT_BUF_GROUP
                     * control to decoder. Only call MPP_DEC_SET_INFO_CHANGE_READY
                     * to let decoder go on. Then decoder will use create buffer
                     * internally and user need to release each frame they get.
                     *
                     * Advantage:
                     * Easy to use and get a demo quickly
                     * Disadvantage:
                     * 1. The buffer from decoder may not be return before
                     * decoder is close. So memroy leak or crash may happen.
                     * 2. The decoder memory usage can not be control. Decoder
                     * is on a free-to-run status and consume all memory it can
                     * get.
                     * 3. Difficult to implement zero-copy display path.
                     *
                     * Mode 2: Half internal mode
                     * This is the mode current test code using. User need to
                     * create MppBufferGroup according to the returned info
                     * change MppFrame. User can use mpp_buffer_group_limit_config
                     * function to limit decoder memory usage.
                     *
                     * Advantage:
                     * 1. Easy to use
                     * 2. User can release MppBufferGroup after decoder is closed.
                     *    So memory can stay longer safely.
                     * 3. Can limit the memory usage by mpp_buffer_group_limit_config
                     * Disadvantage:
                     * 1. The buffer limitation is still not accurate. Memory usage
                     * is 100% fixed.
                     * 2. Also difficult to implement zero-copy display path.
                     *
                     * Mode 3: Pure external mode
                     * In this mode use need to create empty MppBufferGroup and
                     * import memory from external allocator by file handle.
                     * On Android surfaceflinger will create buffer. Then
                     * mediaserver get the file handle from surfaceflinger and
                     * commit to decoder's MppBufferGroup.
                     *
                     * Advantage:
                     * 1. Most efficient way for zero-copy display
                     * Disadvantage:
                     * 1. Difficult to learn and use.
                     * 2. Player work flow may limit this usage.
                     * 3. May need a external parser to get the correct buffer
                     * size for the external allocator.
                     *
                     * The required buffer size caculation:
                     * hor_stride * ver_stride * 3 / 2 for pixel data
                     * hor_stride * ver_stride / 2 for extra info
                     * Total hor_stride * ver_stride * 2 will be enough.
                     *
                     * For H.264/H.265 20+ buffers will be enough.
                     * For other codec 10 buffers will be enough.
                     */
                    ret = mpp_buffer_group_get_internal(&data->frm_grp, MPP_BUFFER_TYPE_ION);
                    if (ret) {
                        mpp_err("get mpp buffer group  failed ret %d\n", ret);
                        break;
                    }
                    mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, data->frm_grp);

                    mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                } else {
                    err_info = mpp_frame_get_errinfo(frame) | mpp_frame_get_discard(frame);
                    if (err_info) {
                        mpp_log("decoder_get_frame get err info:%d discard:%d.\n",
                                mpp_frame_get_errinfo(frame), mpp_frame_get_discard(frame));
                    }
                    RK_U32 width = mpp_frame_get_width(frame);
                    RK_U32 height = mpp_frame_get_height(frame);
                    mpp_log("[%d x %d]decode_get_frame get frame %d\n", width, height, data->frame_count);
                    data->frame_count++;

                    if (data->fp_output && !err_info)
                        dump_mpp_frame_to_file(frame, data->fp_output);
                }
                frm_eos = mpp_frame_get_eos(frame);
                mpp_frame_deinit(&frame);
                frame = NULL;
                get_frm = 1;
            }

            // if last packet is send but last frame is not found continue
            if (pkt_eos && pkt_done && !frm_eos) {
                msleep(10);
                continue;
            }

            if (frm_eos) {
                mpp_log("found last frame\n");
                break;
            }

            if (get_frm)
                continue;
            break;
        } while (1);

        if (pkt_done)
            break;

        /*
         * why sleep here:
         * mpi->decode_put_packet will failed when packet in internal queue is
         * full,waiting the package is consumed .Usually hardware decode one
         * frame which resolution is 1080p needs 2 ms,so here we sleep 3ms
         * * is enough.
         */
        msleep(3);
    } while (1);

    return ret;
}

static int decode_advanced(MpiDecLoopData *data)
{
    RK_U32 pkt_eos  = 0;
    MPP_RET ret = MPP_OK;
    MppCtx ctx  = data->ctx;
    MppApi *mpi = data->mpi;
    char   *buf = data->buf;
    MppPacket packet = data->packet;
    MppFrame  frame  = data->frame;
    MppTask task = NULL;
    size_t read_size = fread(buf, 1, data->packet_size, data->fp_input);

    if (read_size != data->packet_size || feof(data->fp_input)) {
        mpp_log("found last packet\n");

        // setup eos flag
        data->eos = pkt_eos = 1;
    }

    // reset pos
    mpp_packet_set_pos(packet, buf);
    mpp_packet_set_length(packet, read_size);
    // setup eos flag
    if (pkt_eos)
        mpp_packet_set_eos(packet);

    ret = mpi->poll(ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret) {
        mpp_err("mpp input poll failed\n");
        return ret;
    }

    ret = mpi->dequeue(ctx, MPP_PORT_INPUT, &task);  /* input queue */
    if (ret) {
        mpp_err("mpp task input dequeue failed\n");
        return ret;
    }

    mpp_assert(task);

    mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);
    mpp_task_meta_set_frame (task, KEY_OUTPUT_FRAME,  frame);

    ret = mpi->enqueue(ctx, MPP_PORT_INPUT, task);  /* input queue */
    if (ret) {
        mpp_err("mpp task input enqueue failed\n");
        return ret;
    }

    /* poll and wait here */
    ret = mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret) {
        mpp_err("mpp output poll failed\n");
        return ret;
    }

    ret = mpi->dequeue(ctx, MPP_PORT_OUTPUT, &task); /* output queue */
    if (ret) {
        mpp_err("mpp task output dequeue failed\n");
        return ret;
    }

    mpp_assert(task);

    if (task) {
        MppFrame frame_out = NULL;
        mpp_task_meta_get_frame(task, KEY_OUTPUT_FRAME, &frame_out);
        //mpp_assert(packet_out == packet);

        if (frame) {
            /* write frame to file here */
            MppBuffer buf_out = mpp_frame_get_buffer(frame_out);

            if (buf_out) {
                void *ptr = mpp_buffer_get_ptr(buf_out);
                size_t len  = mpp_buffer_get_size(buf_out);

                if (data->fp_output)
                    fwrite(ptr, 1, len, data->fp_output);

                mpp_log("decoded frame %d size %d\n", data->frame_count, len);
            }

            if (mpp_frame_get_eos(frame_out))
                mpp_log("found eos frame\n");
        }

        /* output queue */
        ret = mpi->enqueue(ctx, MPP_PORT_OUTPUT, task);
        if (ret)
            mpp_err("mpp task output enqueue failed\n");
    }

    return ret;
}

void* mpi_dec_test_decode(void *cmd_ctx)
{
    MpiTestCmd *cmd = (MpiTestCmd *)cmd_ctx;
    MPP_RET ret         = MPP_OK;
    size_t file_size    = 0;

    // base flow context
    MppCtx ctx          = NULL;
    MppApi *mpi         = NULL;

    // input / output
    MppPacket packet    = NULL;
    MppFrame  frame     = NULL;

    MpiCmd mpi_cmd      = MPP_CMD_BASE;
    MppParam param      = NULL;
    RK_U32 need_split   = 1;
    RK_U32 output_block = MPP_POLL_BLOCK;
    RK_S64 block_timeout = cmd->timeout;

    // paramter for resource malloc
    RK_U32 width        = cmd->width;
    RK_U32 height       = cmd->height;
    MppCodingType type  = cmd->type;

    // resources
    char *buf           = NULL;
    size_t packet_size  = MPI_DEC_STREAM_SIZE;
    MppBuffer pkt_buf   = NULL;
    MppBuffer frm_buf   = NULL;

    MpiDecLoopData data;

    printf("==>DEC [%d x %d] test start\n", width, height);
    memset(&data, 0, sizeof(data));

    if (cmd->have_input) {
        data.fp_input = fopen(cmd->file_input, "rb");
        if (NULL == data.fp_input) {
            mpp_err("failed to open input file %s\n", cmd->file_input);
            goto MPP_TEST_OUT;
        }

        fseek(data.fp_input, 0L, SEEK_END);
        file_size = ftell(data.fp_input);
        rewind(data.fp_input);
        mpp_log("input file size %ld\n", file_size);
    }

    if (cmd->have_output) {
        char file_name[MAX_FILE_NAME_LENGTH];
        int pthread_id = pthread_self();
        sprintf(file_name, "%s%u", cmd->file_output, pthread_id);
        data.fp_output = fopen(file_name, "w+b");
        if (NULL == data.fp_output) {
            mpp_err("failed to open output file %s\n", file_name);
            goto MPP_TEST_OUT;
        }
    }

    if (cmd->simple) {
        buf = mpp_malloc(char, packet_size);
        if (NULL == buf) {
            mpp_err("mpi_dec_test malloc input stream buffer failed\n");
            goto MPP_TEST_OUT;
        }

        ret = mpp_packet_init(&packet, buf, packet_size);
        if (ret) {
            mpp_err("mpp_packet_init failed\n");
            goto MPP_TEST_OUT;
        }
    } else {
        RK_U32 hor_stride = MPP_ALIGN(width, 16);
        RK_U32 ver_stride = MPP_ALIGN(height, 16);

        ret = mpp_buffer_group_get_internal(&data.frm_grp, MPP_BUFFER_TYPE_ION);
        if (ret) {
            mpp_err("failed to get buffer group for input frame ret %d\n", ret);
            goto MPP_TEST_OUT;
        }

        ret = mpp_buffer_group_get_internal(&data.pkt_grp, MPP_BUFFER_TYPE_ION);
        if (ret) {
            mpp_err("failed to get buffer group for output packet ret %d\n", ret);
            goto MPP_TEST_OUT;
        }

        ret = mpp_frame_init(&frame); /* output frame */
        if (MPP_OK != ret) {
            mpp_err("mpp_frame_init failed\n");
            goto MPP_TEST_OUT;
        }

        /*
         * NOTE: For jpeg could have YUV420 and YUV422 the buffer should be
         * larger for output. And the buffer dimension should align to 16.
         * YUV420 buffer is 3/2 times of w*h.
         * YUV422 buffer is 2 times of w*h.
         * So create larger buffer with 2 times w*h.
         */
        ret = mpp_buffer_get(data.frm_grp, &frm_buf, hor_stride * ver_stride * 2);
        if (ret) {
            mpp_err("failed to get buffer for input frame ret %d\n", ret);
            goto MPP_TEST_OUT;
        }

        // NOTE: for mjpeg decoding send the whole file
        if (type == MPP_VIDEO_CodingMJPEG) {
            packet_size = file_size;
        }

        ret = mpp_buffer_get(data.pkt_grp, &pkt_buf, packet_size);
        if (ret) {
            mpp_err("failed to get buffer for input frame ret %d\n", ret);
            goto MPP_TEST_OUT;
        }
        mpp_packet_init_with_buffer(&packet, pkt_buf);
        buf = mpp_buffer_get_ptr(pkt_buf);

        mpp_frame_set_buffer(frame, frm_buf);
    }

    mpp_log("mpi_dec_test decoder test start w %d h %d type %d\n", width, height, type);

    // decoder demo
    ret = mpp_create(&ctx, &mpi);

    if (MPP_OK != ret) {
        mpp_err("mpp_create failed\n");
        goto MPP_TEST_OUT;
    }

    // NOTE: decoder split mode need to be set before init
    mpi_cmd = MPP_DEC_SET_PARSER_SPLIT_MODE;
    param = &need_split;
    ret = mpi->control(ctx, mpi_cmd, param);
    if (MPP_OK != ret) {
        mpp_err("mpi->control failed\n");
        goto MPP_TEST_OUT;
    }

    if (block_timeout) {
        param = &output_block;
        ret = mpi->control(ctx, MPP_SET_OUTPUT_BLOCK, param);
        if (MPP_OK != ret) {
            mpp_err("Failed to set blocking mode on MPI (code = %d).\n", ret);
            goto MPP_TEST_OUT;
        }

        param = &block_timeout;
        ret = mpi->control(ctx, MPP_SET_OUTPUT_BLOCK_TIMEOUT, param);
        if (MPP_OK != ret) {
            mpp_err("Failed to set blocking mode on MPI (code = %d).\n", ret);
            goto MPP_TEST_OUT;
        }
    }

    ret = mpp_init(ctx, MPP_CTX_DEC, type);
    if (MPP_OK != ret) {
        mpp_err("mpp_init failed\n");
        goto MPP_TEST_OUT;
    }

    data.ctx            = ctx;
    data.mpi            = mpi;
    data.eos            = 0;
    data.buf            = buf;
    data.packet         = packet;
    data.packet_size    = packet_size;
    data.frame          = frame;
    data.frame_count    = 0;

    RK_S64 t_s, t_total_s, t_e, t_diff;
    RK_U32 tmp_frame = 0;
    t_total_s = t_s = mpp_time();
    if (cmd->simple) {
        while (!data.eos) {
            decode_simple(&data);
            t_e = mpp_time();
            t_diff = (t_e - t_s);
            RK_U32 print_condition;
            if (width <= 640)
                print_condition = 1 * 1000 * 1000;
            else
                print_condition = 3 * 1000 * 1000;
            if (t_diff >= print_condition) {
                RK_U32 wink_rate;
                wink_rate = (data.frame_count - tmp_frame) * 1000 * 1000 / t_diff;
                printf("==> DEC [%d x %d] rate=%d\n", width, height, wink_rate);
                t_s = t_e;
                tmp_frame = data.frame_count;
            }
        }
    } else {
        while (!data.eos) {
            decode_advanced(&data);
        }
    }

    t_e = mpp_time();
    RK_U32 *rate = malloc(sizeof(RK_U32));
    mpp_assert(rate != NULL);
    t_diff = (t_e - t_total_s) / 1000;
    mpp_log("decode %d frame use time %lld frm rate %d\n",
            data.frame_count, t_diff, data.frame_count * 1000 / t_diff);
    *rate = (data.frame_count * 1000) / t_diff;

    ret = mpi->reset(ctx);
    if (MPP_OK != ret) {
        mpp_err("mpi->reset failed\n");
        goto MPP_TEST_OUT;
    }

MPP_TEST_OUT:
    if (packet) {
        mpp_packet_deinit(&packet);
        packet = NULL;
    }

    if (frame) {
        mpp_frame_deinit(&frame);
        frame = NULL;
    }

    if (ctx) {
        mpp_destroy(ctx);
        ctx = NULL;
    }

    if (cmd->simple) {
        if (buf) {
            mpp_free(buf);
            buf = NULL;
        }
    } else {
        if (pkt_buf) {
            mpp_buffer_put(pkt_buf);
            pkt_buf = NULL;
        }

        if (frm_buf) {
            mpp_buffer_put(frm_buf);
            frm_buf = NULL;
        }
    }

    if (data.pkt_grp) {
        mpp_buffer_group_put(data.pkt_grp);
        data.pkt_grp = NULL;
    }

    if (data.frm_grp) {
        mpp_buffer_group_put(data.frm_grp);
        data.frm_grp = NULL;
    }

    if (data.fp_output) {
        fclose(data.fp_output);
        data.fp_output = NULL;
    }

    if (data.fp_input) {
        fclose(data.fp_input);
        data.fp_input = NULL;
    }

    return rate;
}

static void mpi_test_help()
{
    mpp_log("usage: mpi_test [options]\n");
    show_options(mpi_multi_cmd);
    mpp_show_support_format();
}

static RK_S32 mpi_test_parse_options(int argc, char **argv, MpiTestParam* pTestParm)
{
    char *opt;
    const char *p;
    RK_S32 optindex = 1;
    RK_S32 err = MPP_NOK;

    if ((argc < 2) || (pTestParm == NULL)) {
        err = 1;
        return err;
    }

    /* parse options */
    while (optindex < argc) {
        char *delims = { ":" };
        RK_S32 index = optindex - 1;
        opt  = argv[optindex];
        opt++;
        p = strtok(opt, delims);
        while (p != NULL) {
            switch (*p) {
            case 'i':
                strncpy(pTestParm->cmd[index].file_input, p + 2, MAX_FILE_NAME_LENGTH);
                pTestParm->cmd[index].file_input[strlen(p + 2)] = '\0';
                pTestParm->cmd[index].have_input = 1;
                break;
            case 'o':
                strncpy(pTestParm->cmd[index].file_output, p + 2, MAX_FILE_NAME_LENGTH);
                pTestParm->cmd[index].file_output[strlen(p + 2)] = '\0';
                pTestParm->cmd[index].have_output = 1;
                break;
            case 'd':
                pTestParm->cmd[index].debug = atoi(p + 2);
                break;
            case 'w':
                pTestParm->cmd[index].width = atoi(p + 2);
                break;
            case 'h':
                pTestParm->cmd[index].height = atoi(p + 2);
                break;
            case 't':
                pTestParm->cmd[index].type = (MppCodingType)atoi(p + 2);
                err = mpp_check_support_format(MPP_CTX_DEC, pTestParm->cmd[index].type);
                if (err) {
                    mpp_err("invalid input coding type\n");
                    goto PARSE_OPINIONS_OUT;
                }
                break;
            case 'n':
                pTestParm->cmd[index].num_frames = atoi(p + 2);
                break;
            case 'x':
                pTestParm->cmd[index].timeout = atoi(p + 2);
                break;
            case 'p':
                pTestParm->cmd[index].nthreads = atoi(p + 2);
                break;
            case 'f':
                pTestParm->cmd[index].format = atoi(p + 2);
                err = ((pTestParm->cmd[index].format >= MPP_FMT_YUV_BUTT &&
                        pTestParm->cmd[index].format < MPP_FRAME_FMT_RGB) ||
                       pTestParm->cmd[index].format >= MPP_FMT_RGB_BUTT);
                if (err) {
                    mpp_err("invalid input format %d\n", pTestParm->cmd[index].format);
                    goto PARSE_OPINIONS_OUT;
                }
                break;
            case 'r':
                pTestParm->cmd[index].rc_mode = atoi(p + 2);
                break;
            case 'q':
                pTestParm->cmd[index].nTestType = atoi(p + 2);
                break;
            default:
                goto PARSE_OPINIONS_OUT;
                break;
            }

            p = strtok(NULL, delims);
        }

        pTestParm->nResoultions = optindex;
        optindex++;
    }

    err = 0;

PARSE_OPINIONS_OUT:
    return err;
}

static void mpi_test_show_options(MpiTestParam* pTestParm)
{
    RK_U32 index = 0;
    for (index = 0; index < pTestParm->nResoultions; index++) {
        mpp_log("cmd parse result:%d\n", index + 1);
        mpp_log("input  file name: %s\n", pTestParm->cmd[index].file_input);
        mpp_log("output file name: %s\n", pTestParm->cmd[index].file_output);
        mpp_log("width      : %4d\n", pTestParm->cmd[index].width);
        mpp_log("height     : %4d\n", pTestParm->cmd[index].height);
        mpp_log("type       : %d\n", pTestParm->cmd[index].type);
        mpp_log("debug flag : %x\n", pTestParm->cmd[index].debug);
        mpp_log("threads       : %d\n", pTestParm->cmd[index].nthreads);
        mpp_log("num frames : %d\n", pTestParm->cmd[index].num_frames);
        mpp_log("format : %d\n", pTestParm->cmd[index].format);
        mpp_log("mpp test type : %d\n", pTestParm->cmd[index].nTestType);
    }
}

int main(int argc, char **argv)
{
    RK_S32 ret = 0;
    RK_U32 total_rate = 0;
    RK_U32 total_thread = 0;
    RK_U32 resoultionIndex = 0;
    RK_U32 handleIndex = 0;
    MpiTestParam testParamer;
    MpiTestParam* pTestParm = &testParamer;
    memset((void*)pTestParm, 0, sizeof(*pTestParm));

    // parse the cmd option
    ret = mpi_test_parse_options(argc, argv, pTestParm);
    if (ret) {
        if (ret < 0) {
            mpp_err("mpi_dec_test_parse_options: input parameter invalid\n");
        }

        mpi_test_help();
        return ret;
    }

    mpi_test_show_options(pTestParm);

    mpp_env_set_u32("mpi_debug", pTestParm->cmd[0].debug);

    for (resoultionIndex = 0; resoultionIndex < pTestParm->nResoultions; resoultionIndex++) {
        total_thread += pTestParm->cmd[resoultionIndex].nthreads;
    }

    pthread_t *handles = malloc(sizeof(pthread_t) * total_thread);
    if (!handles) {
        mpp_log("failed to alloc handles\n");
        return ret;
    }

    RK_U32 **rates = malloc(sizeof(RK_U32 *) * total_thread);
    mpp_assert(rates != NULL);

    for (resoultionIndex = 0; resoultionIndex < pTestParm->nResoultions && handleIndex < total_thread; resoultionIndex++) {

        pTestParm->cmd[resoultionIndex].simple =
            (pTestParm->cmd[resoultionIndex].type != MPP_VIDEO_CodingMJPEG) ? (1) : (0);

        RK_U32 i = 0;

        for (i = 0; i < pTestParm->cmd[resoultionIndex].nthreads; i++) {
            if (pTestParm->cmd[resoultionIndex].nTestType == 1) {// type=1 decode
                ret = pthread_create(&handles[handleIndex], NULL, mpi_dec_test_decode, & pTestParm->cmd[resoultionIndex]);
            } else if (pTestParm->cmd[resoultionIndex].nTestType == 2) {//type=2 encode
                ret = pthread_create(&handles[handleIndex], NULL, mpi_enc_test, & pTestParm->cmd[resoultionIndex]);
            }

            if (ret != 0) {
                mpp_log("failed to create thread\n");
                return ret;
            }
            handleIndex++;
        }
    }

    if (handleIndex != total_thread) {
        printf("mpi dec multi test error.\n");
        return 0;
    }

    for (resoultionIndex = 0, handleIndex = 0; resoultionIndex < pTestParm->nResoultions && handleIndex < total_thread; resoultionIndex++) {
        RK_U32 j = 0;
        RK_U32 resoultion_rate = 0;
        RK_U32 average_rate = 0;

        for (j = 0; j < pTestParm->cmd[resoultionIndex].nthreads; j++) {
            pthread_join(handles[handleIndex], (void *)&rates[handleIndex]);
            resoultion_rate += *rates[handleIndex];
            handleIndex++;
        }
        average_rate = resoultion_rate / pTestParm->cmd[resoultionIndex].nthreads;
        if (pTestParm->cmd[resoultionIndex].nTestType == 1) {
            printf("====>DEC [%d x %d] average frame rate=%d\n", pTestParm->cmd[resoultionIndex].width,
                   pTestParm->cmd[resoultionIndex].height, average_rate);
        } else {
            printf("====>ENC [%d x %d] average frame rate=%d\n", pTestParm->cmd[resoultionIndex].width,
                   pTestParm->cmd[resoultionIndex].height, average_rate);
        }
        total_rate += resoultion_rate;
    }

    total_rate /= total_thread;
    printf("====>total average frame rate=%d\n", total_rate);

    mpp_env_set_u32("mpi_debug", 0x0);
    return total_rate;
}

