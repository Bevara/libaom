/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / AV1 video decoder filter
 *  based on libaom (https://aomedia.googlesource.com/aom/)
 *
 *  Pairs with a demuxer (e.g. webmdmx) that provides framed AV1
 *  temporal units (one or more OBUs per packet, low-overhead
 *  bitstream format) - this filter does not parse a container itself.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;

	aom_codec_ctx_t codec;
	Bool codec_ready;

	u32 width, height;
	Bool is_playing;
} GF_AV1DecCtx;

static GF_Err av1dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_AV1DecCtx *ctx = (GF_AV1DecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->codec_ready)
		{
			aom_codec_destroy(&ctx->codec);
			ctx->codec_ready = GF_FALSE;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));

	if (!ctx->codec_ready)
	{
		aom_codec_err_t res = aom_codec_dec_init(&ctx->codec, aom_codec_av1_dx(), NULL, 0);
		if (res != AOM_CODEC_OK)
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AV1Dec] Failed to init libaom decoder: %s\n", aom_codec_error(&ctx->codec)));
			return GF_IO_ERR;
		}
		ctx->codec_ready = GF_TRUE;
	}

	return GF_OK;
}

static Bool av1dec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_AV1DecCtx *ctx = (GF_AV1DecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static void av1dec_send_frame(GF_AV1DecCtx *ctx, const aom_image_t *img, u64 cts)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 y, c;
	u32 w = img->d_w;
	u32 h = img->d_h;
	u32 cw = (w + img->x_chroma_shift) >> img->x_chroma_shift;
	u32 ch = (h + img->y_chroma_shift) >> img->y_chroma_shift;
	u32 y_size = w * h;
	u32 c_size = cw * ch;
	u32 out_size = y_size + 2 * c_size;

	if ((w != ctx->width) || (h != ctx->height))
	{
		ctx->width = w;
		ctx->height = h;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w));
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck) return;

	for (y = 0; y < h; y++)
	{
		memcpy(output + y * w, img->planes[AOM_PLANE_Y] + y * img->stride[AOM_PLANE_Y], w);
	}
	output += y_size;

	for (c = AOM_PLANE_U; c <= AOM_PLANE_V; c++)
	{
		for (y = 0; y < ch; y++)
		{
			memcpy(output + y * cw, img->planes[c] + y * img->stride[c], cw);
		}
		output += c_size;
	}

	gf_filter_pck_set_cts(dst_pck, cts);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
}

static void av1dec_flush_frames(GF_AV1DecCtx *ctx, u64 cts)
{
	aom_codec_iter_t iter = NULL;
	aom_image_t *img;
	while ((img = aom_codec_get_frame(&ctx->codec, &iter)) != NULL)
	{
		av1dec_send_frame(ctx, img, cts);
	}
}

static GF_Err av1dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size;
	u64 cts;
	aom_codec_err_t res;
	GF_AV1DecCtx *ctx = (GF_AV1DecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}
	cts = gf_filter_pck_get_cts(pck);

	res = aom_codec_decode(&ctx->codec, data, size, NULL);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (res != AOM_CODEC_OK)
	{
		GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[AV1Dec] Failed to decode frame: %s\n", aom_codec_error(&ctx->codec)));
		return GF_OK;
	}

	av1dec_flush_frames(ctx, cts);

	return GF_OK;
}

static void av1dec_finalize(GF_Filter *filter)
{
	GF_AV1DecCtx *ctx = (GF_AV1DecCtx *)gf_filter_get_udta(filter);
	if (ctx->codec_ready)
	{
		aom_codec_destroy(&ctx->codec);
		ctx->codec_ready = GF_FALSE;
	}
}

static const GF_FilterCapability AV1DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_AV1),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister AV1DecoderRegister = {
	.name = "av1dec",
	GF_FS_SET_DESCRIPTION("AV1 video decoder")
		GF_FS_SET_HELP("This filter decodes AV1 video elementary streams using libaom.")
			.private_size = sizeof(GF_AV1DecCtx),
	SETCAPS(AV1DecCaps),
	.configure_pid = av1dec_configure_pid,
	.process = av1dec_process,
	.process_event = av1dec_process_event,
	.finalize = av1dec_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE av1dec_register(GF_FilterSession *session)
{
	return &AV1DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_av1dec(void) {
    gf_filter_auto_register("av1dec", av1dec_register);
}
