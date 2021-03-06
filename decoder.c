/*
 * Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <string.h>
#include "vdpau_private.h"
#include "ve.h"
#include <stdio.h>

VdpStatus vdp_decoder_create(VdpDevice device, VdpDecoderProfile profile, uint32_t width, uint32_t height, uint32_t max_references, VdpDecoder *decoder)
{
    device_ctx_t *dev = handle_get(device);
    if (!dev)
        return VDP_STATUS_INVALID_HANDLE;

    if (max_references > 16)
        return VDP_STATUS_ERROR;

    decoder_ctx_t *dec = handle_create(sizeof(*dec), decoder, htype_decoder);
    if (!dec)
        goto err_ctx;
    
    VDPAU_DBG("vdpau decoder=%d created", *decoder);

    memset(dec, 0, sizeof(*dec));
    dec->device = dev;
    dec->profile = profile;
    dec->width = width;
    dec->height = height;

    dec->data = cedarv_malloc(VBV_SIZE);
    if (! cedarv_isValid(dec->data))
        goto err_data;
    dec->data_pos = 0;

    VdpStatus ret;
    switch (profile)
    {
    case VDP_DECODER_PROFILE_MPEG1:
    case VDP_DECODER_PROFILE_MPEG2_SIMPLE:
    case VDP_DECODER_PROFILE_MPEG2_MAIN:
        cedarv_allocateEngine(CEDARV_ENGINE_MPEG);
        ret = new_decoder_mpeg12(dec);
        break;

    case VDP_DECODER_PROFILE_H264_BASELINE:
    case VDP_DECODER_PROFILE_H264_MAIN:
    case VDP_DECODER_PROFILE_H264_HIGH:
        cedarv_allocateEngine(CEDARV_ENGINE_H264);
        ret = new_decoder_h264(dec);
        break;

    case VDP_DECODER_PROFILE_MPEG4_PART2_SP:
        case VDP_DECODER_PROFILE_MPEG4_PART2_ASP:
    case VDP_DECODER_PROFILE_DIVX4_QMOBILE:
    case VDP_DECODER_PROFILE_DIVX4_MOBILE:
    case VDP_DECODER_PROFILE_DIVX4_HOME_THEATER:
    case VDP_DECODER_PROFILE_DIVX4_HD_1080P:
    case VDP_DECODER_PROFILE_DIVX5_QMOBILE:
    case VDP_DECODER_PROFILE_DIVX5_MOBILE:
    case VDP_DECODER_PROFILE_DIVX5_HOME_THEATER:
    case VDP_DECODER_PROFILE_DIVX5_HD_1080P:
        cedarv_allocateEngine(CEDARV_ENGINE_MPEG);
        ret = new_decoder_mpeg4(dec);
        break;
    case VDP_DECODER_PROFILE_DIVX3_HD_1080P:
    case VDP_DECODER_PROFILE_DIVX3_QMOBILE:
    case VDP_DECODER_PROFILE_DIVX3_MOBILE:
    case VDP_DECODER_PROFILE_DIVX3_HOME_THEATER:
        cedarv_allocateEngine(CEDARV_ENGINE_MPEG);
        ret = new_decoder_msmpeg4(dec);
        break;
    default:
        ret = VDP_STATUS_INVALID_DECODER_PROFILE;
        break;
    }

    if (ret != VDP_STATUS_OK)
        goto err_decoder;

    handle_release(device);
    return VDP_STATUS_OK;

err_handle:
    if (dec->private_free)
        dec->private_free(dec);
err_decoder:
    cedarv_free(dec->data);
    cedarv_freeEngine();
err_data:
    handle_destroy(*decoder);
err_ctx:
    handle_release(device);
    return VDP_STATUS_RESOURCES;
}

VdpStatus vdp_decoder_destroy(VdpDecoder decoder)
{
    VDPAU_DBG("vdpau decoder=%d destroyed", decoder);
    decoder_ctx_t *dec = handle_get(decoder);
    if (!dec)
        return VDP_STATUS_INVALID_HANDLE;

    if (dec->private_free)
        dec->private_free(dec);

    cedarv_free(dec->data);
    cedarv_freeEngine();

    handle_release(decoder);
    handle_destroy(decoder);

    return VDP_STATUS_OK;
}

VdpStatus vdp_decoder_get_parameters(VdpDecoder decoder, VdpDecoderProfile *profile, uint32_t *width, uint32_t *height)
{
    decoder_ctx_t *dec = handle_get(decoder);
    if (!dec)
        return VDP_STATUS_INVALID_HANDLE;

    if (profile)
        *profile = dec->profile;

    if (width)
        *width = dec->width;

    if (height)
        *height = dec->height;
    
    handle_release(decoder);

    return VDP_STATUS_OK;
}

VdpStatus vdp_decoder_render(VdpDecoder decoder, VdpVideoSurface target, VdpPictureInfo const *picture_info, uint32_t bitstream_buffer_count, VdpBitstreamBuffer const *bitstream_buffers)
{
    VdpStatus status = VDP_STATUS_INVALID_HANDLE;
    decoder_ctx_t *dec = handle_get(decoder);
    if (!dec)
        return VDP_STATUS_INVALID_HANDLE;

    video_surface_ctx_t *vid = handle_get(target);
    if (!vid)
    {
        handle_release(decoder);
        return VDP_STATUS_INVALID_HANDLE;
    }

    vid->source_format = INTERNAL_YCBCR_FORMAT;
    unsigned int i, pos = 0;

    for (i = 0; i < bitstream_buffer_count; i++)
    {
        cedarv_memcpy(dec->data, pos, bitstream_buffers[i].bitstream, bitstream_buffers[i].bitstream_bytes);
        pos += bitstream_buffers[i].bitstream_bytes;
    }
    //memory is mapped unchached, therefore no flush necessary. hopefully ;)
    cedarv_flush_cache(dec->data, pos);

    status = dec->decode(dec, picture_info, pos, vid);

    handle_release(target);
    handle_release(decoder);
    return status;
}

VdpStatus vdp_decoder_query_capabilities(VdpDevice device, VdpDecoderProfile profile, VdpBool *is_supported, uint32_t *max_level, uint32_t *max_macroblocks, uint32_t *max_width, uint32_t *max_height)
{
    if (!is_supported || !max_level || !max_macroblocks || !max_width || !max_height)
        return VDP_STATUS_INVALID_POINTER;

    device_ctx_t *dev = handle_get(device);
    if (!dev)
        return VDP_STATUS_INVALID_HANDLE;

    // guessed in lack of documentation, bigger pictures should be possible
    *max_level = 16;
    *max_width = 3840;
    *max_height = 2160;
    *max_macroblocks = (*max_width * *max_height) / (16 * 16);

    switch (profile)
    {
    case VDP_DECODER_PROFILE_MPEG1:
    case VDP_DECODER_PROFILE_MPEG2_SIMPLE:
    case VDP_DECODER_PROFILE_MPEG2_MAIN:
    case VDP_DECODER_PROFILE_H264_BASELINE:
    case VDP_DECODER_PROFILE_H264_MAIN:
    case VDP_DECODER_PROFILE_H264_HIGH:
    case VDP_DECODER_PROFILE_MPEG4_PART2_SP:
    case VDP_DECODER_PROFILE_MPEG4_PART2_ASP:    
    case VDP_DECODER_PROFILE_DIVX4_QMOBILE:
    case VDP_DECODER_PROFILE_DIVX4_MOBILE:
    case VDP_DECODER_PROFILE_DIVX4_HOME_THEATER:
    case VDP_DECODER_PROFILE_DIVX4_HD_1080P:
    case VDP_DECODER_PROFILE_DIVX3_HD_1080P:
        *is_supported = VDP_TRUE;
        break;

    default:
        *is_supported = VDP_FALSE;
        break;
    }

    handle_release(device);
    return VDP_STATUS_OK;
}

VdpStatus vdp_decoder_set_video_control_data(VdpDecoder decoder, VdpDecoderControlDataId id, VdpDecoderControlData *data)
{
    VdpStatus status = VDP_STATUS_OK; 
    decoder_ctx_t *dec = handle_get(decoder);
    if (!dec)
        return VDP_STATUS_INVALID_HANDLE;
    if (dec->setVideoControlData)
    {
        status = dec->setVideoControlData(dec, id, data);
    }
    handle_release(decoder);
    return status;
}

#if 1
VdpStatus vdp_decoder_render_stream(VdpDecoder decoder, VdpVideoSurface target, VdpPictureInfo const *picture_info, uint32_t bitstream_buffer_count, VdpBitstreamBuffer const *bitstream_buffers, uint32_t* bitstream_pos_returned)
{
	decoder_ctx_t *dec = handle_get(decoder);
	if (!dec)
		return VDP_STATUS_INVALID_HANDLE;

	video_surface_ctx_t *vid = handle_get(target);
	if (!vid)
		return VDP_STATUS_INVALID_HANDLE;

	vid->source_format = INTERNAL_YCBCR_FORMAT;
	unsigned int i, pos = dec->data_pos;

	for (i = 0; i < bitstream_buffer_count; i++)
	{
		cedarv_memcpy(dec->data, pos, bitstream_buffers[i].bitstream, bitstream_buffers[i].bitstream_bytes);
		pos += bitstream_buffers[i].bitstream_bytes;
	}
	//memory is mapped unchached, therefore no flush necessary. hopefully ;)
	cedarv_flush_cache(dec->data, pos);

	int error = dec->decode_stream(dec, picture_info, pos, vid, bitstream_pos_returned);
	if(error)
	{
		dec->data_pos = (*bitstream_pos_returned + 7) >> 3;
		if(dec->data_pos > pos)
		{
			dec->data_pos = pos;
		}
		//memmove(dec->data, (char*)dec->data + dec->data_pos, pos - dec->data_pos);
	}
	else
	{
		dec->data_pos = 0;
	}
	return error;
}
#endif
